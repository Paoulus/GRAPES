/*
 *  Copyright (c) 2010 Luca Abeni
 *  Copyright (c) 2010 Csaba Kiraly
 *  Copyright (c) 2010 Alessandro Russo
 *
 *  This is free software; see lgpl-2.1.txt
 */

#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#else
#define _WIN32_WINNT 0x0501 /* WINNT>=0x501 (WindowsXP) for supporting getaddrinfo/freeaddrinfo.*/
#include "win32-net.h"
#endif

#include "net_helper.h"
#include "fragmenter.h"

#define MAX_MSG_SIZE 1024 * 60
enum L3PROTOCOL {IPv4, IPv6} l3 = IPv4;

struct nodeID {
  struct sockaddr_storage addr;
  int fd;
};

struct net_helper_context {
	struct fragmenter* incoming_frag;
	struct fragmenter* outgoing_frag;
} net_helper_ctx;

int wait4data(const struct nodeID *s, struct timeval *tout, int *user_fds)
{
  fd_set fds;
  int i, res, max_fd;

  FD_ZERO(&fds);
  if (s) {
    max_fd = s->fd;
    FD_SET(s->fd, &fds);
  } else {
    max_fd = -1;
  }
  if (user_fds) {
    for (i = 0; user_fds[i] != -1; i++) {
      FD_SET(user_fds[i], &fds);
      if (user_fds[i] > max_fd) {
        max_fd = user_fds[i];
      }
    }
  }
  res = select(max_fd + 1, &fds, NULL, NULL, tout);
	
  if (res <= 0) {
    return res;
  }
  if (s && FD_ISSET(s->fd, &fds)) {
    return 1;
  }

  /* If execution arrives here, user_fds cannot be 0
     (an FD is ready, and it's not s->fd) */
  for (i = 0; user_fds[i] != -1; i++) {
    if (!FD_ISSET(user_fds[i], &fds)) {
      user_fds[i] = -2;
    }
  }

  return 2;
}

struct nodeID *create_node(const char *IPaddr, int port)
{
  struct nodeID *s;
  int res;
  struct addrinfo hints, *result;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICHOST;

  s = malloc(sizeof(struct nodeID));
  memset(s, 0, sizeof(struct nodeID));

  if ((res = getaddrinfo(IPaddr, NULL, &hints, &result)))
  {
    fprintf(stderr, "Cannot resolve hostname '%s'\n", IPaddr);
    return NULL;
  }
  s->addr.ss_family = result->ai_family;
  switch (result->ai_family)
  {
    case (AF_INET):
      ((struct sockaddr_in *)&s->addr)->sin_port = htons(port);
      res = inet_pton (result->ai_family, IPaddr, &((struct sockaddr_in *)&s->addr)->sin_addr);
    break;
    case (AF_INET6):
      ((struct sockaddr_in6 *)&s->addr)->sin6_port = htons(port);
      res = inet_pton (result->ai_family, IPaddr, &(((struct sockaddr_in6 *) &s->addr)->sin6_addr));
    break;
    default:
      fprintf(stderr, "Cannot resolve address family %d for '%s'\n", result->ai_family, IPaddr);
      res = 0;
      break;
  }
  freeaddrinfo(result);


  if (res != 1)
  {
    fprintf(stderr, "Could not convert address '%s'\n", IPaddr);
    free(s);

    return NULL;
  }

  s->fd = -1;

  return s;
}

struct nodeID *net_helper_init(const char *my_addr, int port, const char *config)
{
  int res;
  struct nodeID *myself;

  myself = create_node(my_addr, port);
  if (myself == NULL) {
    fprintf(stderr, "Error creating my socket (%s:%d)!\n", my_addr, port);

    return NULL;
  }
  myself->fd =  socket(myself->addr.ss_family, SOCK_DGRAM, 0);
	net_helper_ctx.incoming_frag = fragmenter_init();
	net_helper_ctx.outgoing_frag = fragmenter_init();

  if (myself->fd < 0 || !net_helper_ctx.outgoing_frag ||!net_helper_ctx.incoming_frag ) {
    free(myself);

    return NULL;
  }
//  TODO:
//  if (addr->sa_family == AF_INET6) {
//      r = setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
//  }

  fprintf(stderr, "My sock: %d\n", myself->fd);

  switch (myself->addr.ss_family)
  {
    case (AF_INET):
        res = bind(myself->fd, (struct sockaddr *)&myself->addr, sizeof(struct sockaddr_in));
    break;
    case (AF_INET6):
        res = bind(myself->fd, (struct sockaddr *)&myself->addr, sizeof(struct sockaddr_in6));
    break;
    default:
      fprintf(stderr, "Cannot resolve address family %d in bind\n", myself->addr.ss_family);
      res = 0;
    break;
  }

  if (res < 0) {
    /* bind failed: not a local address... Just close the socket! */
    close(myself->fd);
    free(myself);

    return NULL;
  }

  return myself;
}

void bind_msg_type (uint8_t msgtype)
{
}

void print_hex(const uint8_t *data,const int bytes)
{
	int i;
	fprintf(stderr,"Data: [");
	for (i=0;i<bytes;i++)
		fprintf(stderr,"%02x",data[i]);
	fprintf(stderr,"]\n");
}

int send_to_peer(const struct nodeID *from,const  struct nodeID *to, const uint8_t *buffer_ptr, int buffer_size)
{
	uint16_t msg_id;
	uint8_t i;
	struct fragmenter * frag = net_helper_ctx.outgoing_frag;
	struct msghdr * frag_msg;

	fprintf(stderr,"total bytes: %d\n",buffer_size);
	print_hex(buffer_ptr,buffer_size);
	msg_id = fragmenter_add_msg(frag,buffer_ptr,buffer_size,6);

	for(i=0; i<fragmenter_frags_num(frag,msg_id);i++)
	{
		frag_msg = fragmenter_get_frag(frag,msg_id,i);
		fprintf(stderr,"bytes: %d\n",frag_msg->msg_iov[1].iov_len);
		print_hex(frag_msg->msg_iov[1].iov_base,frag_msg->msg_iov[1].iov_len);
	  frag_msg->msg_namelen = sizeof(struct sockaddr_storage);
  	frag_msg->msg_name = &to->addr;
    if(sendmsg(from->fd, frag_msg, 0) < 0)
      fprintf(stderr,"net-helper: sendmsg failed errno %d: %s\n", errno, strerror(errno));
	}
	return 0;
}

int recv_from_peer(const struct nodeID *local, struct nodeID **remote, uint8_t *buffer_ptr, int buffer_size)
{
	int res,next_res;
	struct msghdr frag_msg;
	struct fragmenter * frag = net_helper_ctx.incoming_frag;

	do {
	fragmenter_frag_init(&frag_msg,0,0,0,NULL,buffer_size,FRAG_DATA);
  res = recvmsg(local->fd, &frag_msg, 0);
	fragmenter_frag_shrink(&frag_msg,res);
//	print_hex(frag_msg.msg_iov[1].iov_base,buffer_size);//frag_msg.msg_iov[1].iov_len);
		fprintf(stderr,"frag bytes: %d\n",frag_msg.msg_iov[1].iov_len);
	print_hex(frag_msg.msg_iov[1].iov_base,frag_msg.msg_iov[1].iov_len);
	if (res > 0)
		fragmenter_add_frag(frag,&frag_msg);
//	fragmenter_frag_deinit(&frag_msg);
//
		ioctl(local->fd,FIONREAD,&next_res);
	} while (next_res);
		
	res = fragmenter_pop_msg(frag,buffer_ptr,buffer_size);
	fprintf(stderr,"received bytes %d \n",res);
	print_hex(buffer_ptr,res);
	return res;

/*	//////////////////////////////////
  int res, recv, m_seq, frag_seq;
  struct sockaddr_storage raddr;
  struct msghdr msg = {0};
  static struct my_hdr_t my_hdr;
  struct iovec iov[2];

  iov[0].iov_base = &my_hdr;
  iov[0].iov_len = sizeof(struct my_hdr_t);
  msg.msg_name = &raddr;
  msg.msg_namelen = sizeof(struct sockaddr_storage);
  msg.msg_iovlen = 2;
  msg.msg_iov = iov;

  *remote = malloc(sizeof(struct nodeID));
  if (*remote == NULL) {
    return -1;
  }

  recv = 0;
  m_seq = -1;
  frag_seq = 0;
  do {
    iov[1].iov_base = buffer_ptr;
    if (buffer_size > MAX_MSG_SIZE) {
      iov[1].iov_len = MAX_MSG_SIZE;
    } else {
      iov[1].iov_len = buffer_size;
    }
    buffer_size -= iov[1].iov_len;
    buffer_ptr += iov[1].iov_len;
    res = recvmsg(local->fd, &msg, 0);
	fprintf(stderr,"recv buffer content: %s\n",msg.msg_iov[1].iov_base);
		fprintf(stderr,"received bytes %d \n",res);
    recv += (res - sizeof(struct my_hdr_t));
    if (m_seq != -1 && my_hdr.message_id != m_seq) {
      return -1;
    } else {
      m_seq = my_hdr.message_id;
    }
    if (my_hdr.frag_seq != frag_seq + 1) {
      return -1;
    } else {
     frag_seq++;
    }
  } while ((my_hdr.frag_seq < my_hdr.frags_num) && (buffer_size > 0));
  memcpy(&(*remote)->addr, &raddr, msg.msg_namelen);
  (*remote)->fd = -1;

  return recv;
	*/
}

int node_addr(const struct nodeID *s, char *addr, int len)
{
  int n;

  if (node_ip(s, addr, len) < 0) {
    return -1;
  }
  n = snprintf(addr + strlen(addr), len - strlen(addr) - 1, ":%d", node_port(s));

  return n;
}

struct nodeID *nodeid_dup(const struct nodeID *s)
{
  struct nodeID *res;

  res = malloc(sizeof(struct nodeID));
  if (res != NULL) {
    memcpy(res, s, sizeof(struct nodeID));
  }

  return res;
}

int nodeid_equal(const struct nodeID *s1, const struct nodeID *s2)
{
	return (nodeid_cmp(s1,s2) == 0);
//  return (memcmp(&s1->addr, &s2->addr, sizeof(struct sockaddr_storage)) == 0);
}

int nodeid_cmp(const struct nodeID *s1, const struct nodeID *s2)
{
	char ip1[80], ip2[80];
	int port1,port2,res;
	port1=node_port(s1);
	port2=node_port(s2);
	node_ip(s1,ip1,80);
	node_ip(s2,ip2,80);

//	int res = (port1^port2)|strcmp(ip1,ip2);
//	fprintf(stderr,"Comparing %s:%d and %s:%d\n",ip1,port1,ip2,port2);
//	fprintf(stderr,"Result: %d\n",res);
	res  = strcmp(ip1,ip2);
	if (res!=0)
		return res;
	else
		return port1-port2;

//  return memcmp(&s1->addr, &s2->addr, sizeof(struct sockaddr_storage));
}

int nodeid_dump(uint8_t *b, const struct nodeID *s, size_t max_write_size)
{
  if (max_write_size < sizeof(struct sockaddr_storage)) return -1;

  memcpy(b, &s->addr, sizeof(struct sockaddr_storage));

  return sizeof(struct sockaddr_storage);
}

struct nodeID *nodeid_undump(const uint8_t *b, int *len)
{
  struct nodeID *res;
  res = malloc(sizeof(struct nodeID));
  if (res != NULL) {
    memcpy(&res->addr, b, sizeof(struct sockaddr_storage));
    res->fd = -1;
  }
  *len = sizeof(struct sockaddr_storage);

  return res;
}

void nodeid_free(struct nodeID *s)
{
  free(s);
}

int node_ip(const struct nodeID *s, char *ip, int len)
{
  switch (s->addr.ss_family)
  {
    case AF_INET:
      inet_ntop(s->addr.ss_family, &((const struct sockaddr_in *)&s->addr)->sin_addr, ip, len);
      break;
    case AF_INET6:
      inet_ntop(s->addr.ss_family, &((const struct sockaddr_in6 *)&s->addr)->sin6_addr, ip, len);
      break;
    default:
      break;
  }
  if (!ip) {
	  perror("inet_ntop");
	  return -1;
  }
  return 1;
}

int node_port(const struct nodeID *s)
{
  int res;
  switch (s->addr.ss_family)
  {
    case AF_INET:
      res = ntohs(((const struct sockaddr_in *) &s->addr)->sin_port);
      break;
    case AF_INET6:
      res = ntohs(((const struct sockaddr_in6 *)&s->addr)->sin6_port);
      break;
    default:
      res = -1;
      break;
  }
  return res;
}
