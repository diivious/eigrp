// SPDX-License-Identifier: GPL-2.0-or-later
/* Local shared-segment broker for Unix EIGRP UUTs. Copyright (C) 2026 Donnie V. Savage */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "eigrp_unix_wire.h"
typedef struct endpoint { int fd; uint8_t afi; uint32_t ifindex; char segment[EIGRP_UNIX_WIRE_NAME_MAX+1], ifname[EIGRP_UNIX_WIRE_NAME_MAX+1]; uint8_t address[16]; struct endpoint *next; } endpoint_t;
typedef struct client { int fd; struct client *next; } client_t;
static volatile sig_atomic_t stop; static client_t *clients; static endpoint_t *endpoints;
static void on_signal(int sig){(void)sig;stop=1;}
static void client_drop(int fd){client_t **c,*old;endpoint_t **e,*eo;for(e=&endpoints;*e;)if((*e)->fd==fd){eo=*e;*e=eo->next;free(eo);}else e=&(*e)->next;for(c=&clients;*c;c=&(*c)->next)if((*c)->fd==fd){old=*c;*c=old->next;close(old->fd);free(old);return;}}
static bool same_addr(uint8_t afi,const uint8_t*a,const uint8_t*b){return memcmp(a,b,afi==EIGRP_AFI_IPV4?4:16)==0;}
static void register_endpoint(int fd,const eigrp_unix_wire_header_t*h,const char*seg,const char*ifname){endpoint_t*e;for(e=endpoints;e;e=e->next)if(e->fd==fd&&e->afi==h->afi&&strcmp(e->segment,seg)==0&&strcmp(e->ifname,ifname)==0&&same_addr(h->afi,e->address,h->source))return;e=calloc(1,sizeof(*e));if(!e)return;e->fd=fd;e->afi=h->afi;e->ifindex=ntohl(h->ifindex);strcpy(e->segment,seg);strcpy(e->ifname,ifname);memcpy(e->address,h->source,16);e->next=endpoints;endpoints=e;}
/* This forwarding function is the deliberate future fault-injection boundary. */
static void deliver(int source_fd,const uint8_t*frame,size_t length,const eigrp_unix_wire_header_t*h,const char*seg){endpoint_t*e,*prior;bool mc=(h->flags&EIGRP_UNIX_WIRE_FLAG_MULTICAST)!=0;for(e=endpoints;e;e=e->next){eigrp_unix_wire_header_t out;bool duplicate=false;if(e->fd==source_fd||e->afi!=h->afi||strcmp(e->segment,seg)!=0)continue;if(!mc&&!same_addr(h->afi,e->address,h->destination))continue;if(mc){for(prior=endpoints;prior!=e;prior=prior->next)if(prior->fd==e->fd&&prior->afi==e->afi&&prior->ifindex==e->ifindex&&strcmp(prior->segment,e->segment)==0){duplicate=true;break;}if(duplicate)continue;}memcpy(&out,h,sizeof(out));out.ifindex=htonl(e->ifindex);uint8_t *copy=malloc(length);if(!copy)continue;memcpy(copy,frame,length);memcpy(copy,&out,sizeof(out));(void)send(e->fd,copy,length,MSG_NOSIGNAL);free(copy);}}
static int process_frame(int fd){uint8_t frame[sizeof(eigrp_unix_wire_header_t)+2*EIGRP_UNIX_WIRE_NAME_MAX+EIGRP_UNIX_WIRE_PACKET_MAX];eigrp_unix_wire_header_t*h=(void*)frame;ssize_t n=recv(fd,frame,sizeof(frame),0);size_t sl,il,pl,off;char seg[EIGRP_UNIX_WIRE_NAME_MAX+1],ifname[EIGRP_UNIX_WIRE_NAME_MAX+1];if(n<=0)return -1;if(n<(ssize_t)sizeof(*h)||h->version!=EIGRP_UNIX_WIRE_VERSION)return -1;sl=ntohs(h->segment_length);il=ntohs(h->interface_length);pl=ntohs(h->payload_length);if(sl==0||sl>EIGRP_UNIX_WIRE_NAME_MAX||il==0||il>EIGRP_UNIX_WIRE_NAME_MAX)return -1;off=sizeof(*h)+sl+il;if(off+pl!=(size_t)n)return -1;memcpy(seg,frame+sizeof(*h),sl);seg[sl]=0;memcpy(ifname,frame+sizeof(*h)+sl,il);ifname[il]=0;if(h->type==EIGRP_UNIX_WIRE_REGISTER){if(pl)return -1;register_endpoint(fd,h,seg,ifname);return 0;}if(h->type==EIGRP_UNIX_WIRE_PACKET){deliver(fd,frame,(size_t)n,h,seg);return 0;}return -1;}
int main(int argc,char**argv){const char*path=argc>1?argv[1]:EIGRP_UNIX_WIRE_DEFAULT_SOCKET;struct sockaddr_un a={0};int listener;if(strlen(path)>=sizeof(a.sun_path)){fprintf(stderr,"wire socket path too long\n");return 2;}signal(SIGINT,on_signal);signal(SIGTERM,on_signal);listener=socket(AF_UNIX,SOCK_SEQPACKET,0);if(listener<0){perror("socket");return 1;}unlink(path);a.sun_family=AF_UNIX;strcpy(a.sun_path,path);if(bind(listener,(struct sockaddr*)&a,sizeof(a))<0||listen(listener,32)<0){perror("wire broker");close(listener);return 1;}while(!stop){size_t count=1,i=1;client_t*c;struct pollfd*p;for(c=clients;c;c=c->next)count++;p=calloc(count,sizeof(*p));if(!p)break;p[0]=(struct pollfd){listener,POLLIN,0};for(c=clients;c;c=c->next)p[i++]=(struct pollfd){c->fd,POLLIN,0};if(poll(p,count,250)>0){if(p[0].revents&POLLIN){int fd=accept(listener,NULL,NULL);if(fd>=0){c=calloc(1,sizeof(*c));if(c){c->fd=fd;c->next=clients;clients=c;}else close(fd);}}for(i=1;i<count;i++)if(p[i].revents&&(process_frame(p[i].fd)<0||(p[i].revents&(POLLHUP|POLLERR|POLLNVAL))))client_drop(p[i].fd);}free(p);}while(clients)client_drop(clients->fd);close(listener);unlink(path);return 0;}
