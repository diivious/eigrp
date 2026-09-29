// SPDX-License-Identifier: GPL-2.0-or-later
/* Unix virtual wire client transport. Copyright (C) 2026 Donnie V. Savage */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "eigrp_sys.h"
#include "eigrp_unix.h"
#include "eigrp_unix_interface.h"
#include "eigrp_unix_segment.h"
#include "eigrp_unix_wire.h"

typedef struct wire_binding { eigrp_instance_t *eigrp; int fd; struct wire_binding *next; } wire_binding_t;

static ssize_t wire_send_all(int fd, const uint8_t *buffer, size_t length)
{
 size_t offset = 0;
 while(offset < length){ ssize_t n=send(fd,buffer+offset,length-offset,MSG_NOSIGNAL); if(n<0){if(errno==EINTR)continue;return -1;} if(n==0)return -1; offset+=(size_t)n; }
 return (ssize_t)offset;
}

static ssize_t wire_recv_all(int fd, uint8_t *buffer, size_t length)
{
 size_t offset = 0;
 while(offset < length){ ssize_t n=recv(fd,buffer+offset,length-offset,0); if(n<0){if(errno==EINTR)continue;return -1;} if(n==0)return offset==0?0:-1; offset+=(size_t)n; }
 return (ssize_t)offset;
}
static wire_binding_t *bindings;
static wire_binding_t *binding_find(eigrp_instance_t *e){ wire_binding_t *b; for(b=bindings;b;b=b->next) if(b->eigrp==e)return b; return NULL; }
static bool multicast(const eigrp_address_t *a){ if(!a)return false; if(a->afi==EIGRP_AFI_IPV4)return (a->bytes[0]&0xf0U)==0xe0U; if(a->afi==EIGRP_AFI_IPV6)return a->bytes[0]==0xffU; return false; }
static int frame_send(int fd,uint8_t type,eigrp_afi_t afi,eigrp_ifindex_t ifindex,const char *seg,const char *ifname,const eigrp_address_t *src,const eigrp_address_t *dst,const uint8_t *payload,size_t len){
 eigrp_unix_wire_header_t h={0}; size_t sl=strlen(seg), il=strlen(ifname), total; uint8_t *frame, *cursor;
 if(sl>EIGRP_UNIX_WIRE_NAME_MAX||il>EIGRP_UNIX_WIRE_NAME_MAX||len>EIGRP_UNIX_WIRE_PACKET_MAX)return -1;
 h.version=EIGRP_UNIX_WIRE_VERSION;h.type=type;h.afi=(uint8_t)afi;h.flags=dst&&multicast(dst)?EIGRP_UNIX_WIRE_FLAG_MULTICAST:0;h.ifindex=htonl(ifindex);h.segment_length=htons((uint16_t)sl);h.interface_length=htons((uint16_t)il);h.payload_length=htons((uint16_t)len); if(src)memcpy(h.source,src->bytes,16);if(dst)memcpy(h.destination,dst->bytes,16);
 total=sizeof(h)+sl+il+len; frame=malloc(total); if(!frame)return -1; cursor=frame; memcpy(cursor,&h,sizeof(h));cursor+=sizeof(h);memcpy(cursor,seg,sl);cursor+=sl;memcpy(cursor,ifname,il);cursor+=il;if(len)memcpy(cursor,payload,len);
 if(wire_send_all(fd,frame,total)!=(ssize_t)total){free(frame);return -1;} free(frame);return (int)len;
}
typedef struct reg_ctx { int fd; eigrp_afi_t afi; eigrp_ifindex_t ifindex; const char *seg,*ifname; } reg_ctx_t;
static void register_address(const eigrp_prefix_t *p,bool secondary,void *arg){ reg_ctx_t *c=arg; (void)secondary; if(p->address.afi==c->afi)(void)frame_send(c->fd,EIGRP_UNIX_WIRE_REGISTER,c->afi,c->ifindex,c->seg,c->ifname,&p->address,NULL,NULL,0); }
static void register_interface(wire_binding_t *b,eigrp_intf_t *ei){ const char *name=eigrp_intf_name(ei); eigrp_unix_interface_t *ui=eigrp_unix_interface_find(name); eigrp_unix_segment_t *s=eigrp_unix_segment_interface_find(ui); reg_ctx_t c; if(!ui||!s)return; c=(reg_ctx_t){b->fd,eigrp_instance_afi(b->eigrp),eigrp_unix_interface_ifindex(ui),eigrp_unix_segment_name(s),name}; eigrp_unix_interface_address_walk(ui,register_address,&c); }
eigrp_result_t eigrp_sys_socket_open(eigrp_instance_t *e){ struct sockaddr_un a={0}; wire_binding_t *b; const char *path=getenv("EIGRP_UNIX_WIRE_SOCKET"); int fd; if(!e)return EIGRP_RESULT_INVALID_ARGUMENT;if(binding_find(e))return EIGRP_RESULT_SUCCESS;if(!path||!path[0])path=EIGRP_UNIX_WIRE_DEFAULT_SOCKET; if(strlen(path)>=sizeof(a.sun_path))return EIGRP_RESULT_INVALID_ARGUMENT; fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return EIGRP_RESULT_INTERNAL_FAILURE;a.sun_family=AF_UNIX;strcpy(a.sun_path,path);if(connect(fd,(struct sockaddr*)&a,sizeof(a))<0){close(fd);return EIGRP_RESULT_INTERNAL_FAILURE;} b=calloc(1,sizeof(*b));if(!b){close(fd);return EIGRP_RESULT_INTERNAL_FAILURE;}b->eigrp=e;b->fd=fd;b->next=bindings;bindings=b;eigrp_unix_runtime_fd_set(e,fd);return EIGRP_RESULT_SUCCESS; }
void eigrp_sys_socket_close(eigrp_instance_t *e){ wire_binding_t **p,*b;if(!e)return;for(p=&bindings;*p;p=&(*p)->next)if((*p)->eigrp==e){b=*p;*p=b->next;eigrp_unix_runtime_fd_clear(e);close(b->fd);free(b);return;} }
void eigrp_sys_socket_send_buffer_ensure(eigrp_instance_t *e,uint32_t minimum){(void)e;(void)minimum;}
int eigrp_sys_multicast_interface_update(eigrp_operation_t op,eigrp_instance_t *e,eigrp_intf_t *ei){(void)op;(void)e;(void)ei;return 0;}
int eigrp_sys_multicast_join(eigrp_instance_t *e,eigrp_intf_t *ei){wire_binding_t*b=binding_find(e);if(!b)return -1;register_interface(b,ei);return 0;}
int eigrp_sys_multicast_leave(eigrp_instance_t *e,eigrp_intf_t *ei){(void)e;(void)ei;return 0;}
typedef struct source_ctx { eigrp_afi_t afi; eigrp_address_t address; bool found; } source_ctx_t;
static void source_select(const eigrp_prefix_t *p,bool secondary,void *arg){ source_ctx_t*c=arg;(void)secondary;if(c->found||p->address.afi!=c->afi)return;if(c->afi==EIGRP_AFI_IPV6&&(p->address.bytes[0]!=0xfe||(p->address.bytes[1]&0xc0U)!=0x80U))return;c->address=p->address;c->found=true;}
int eigrp_sys_packet_send(eigrp_instance_t *e,eigrp_intf_t *ei,const eigrp_address_t *dst,const uint8_t *payload,size_t len){ wire_binding_t*b=binding_find(e); eigrp_unix_interface_t*ui; eigrp_unix_segment_t*s; eigrp_prefix_t p; source_ctx_t source={0}; if(!b||!ei||!dst||!payload)return -1;ui=eigrp_unix_interface_find(eigrp_intf_name(ei));s=eigrp_unix_segment_interface_find(ui);if(!ui||!s)return -1;register_interface(b,ei);source.afi=eigrp_instance_afi(e);eigrp_unix_interface_address_walk(ui,source_select,&source);if(!source.found){if(eigrp_intf_address_read(ei,&p)!=EIGRP_RESULT_SUCCESS)return -1;source.address=p.address;}return frame_send(b->fd,EIGRP_UNIX_WIRE_PACKET,eigrp_instance_afi(e),eigrp_intf_ifindex(ei),eigrp_unix_segment_name(s),eigrp_intf_name(ei),&source.address,dst,payload,len); }
bool eigrp_sys_packet_receive(eigrp_instance_t *e,uint8_t *buffer,size_t cap,size_t *rlen,eigrp_ifindex_t *ifindex,eigrp_address_t *src,eigrp_address_t *dst,eigrp_packet_rx_meta_t *meta){ wire_binding_t*b=binding_find(e); uint8_t frame[sizeof(eigrp_unix_wire_header_t)+2*EIGRP_UNIX_WIRE_NAME_MAX+EIGRP_UNIX_WIRE_PACKET_MAX]; eigrp_unix_wire_header_t*h=(void*)frame; size_t off,sl,il,pl,body;if(!b||!buffer||!rlen||!ifindex||!src||!dst||!meta)return false;if(wire_recv_all(b->fd,frame,sizeof(*h))!=(ssize_t)sizeof(*h)||h->version!=EIGRP_UNIX_WIRE_VERSION||h->type!=EIGRP_UNIX_WIRE_PACKET)return false;sl=ntohs(h->segment_length);il=ntohs(h->interface_length);pl=ntohs(h->payload_length);if(sl>EIGRP_UNIX_WIRE_NAME_MAX||il>EIGRP_UNIX_WIRE_NAME_MAX||pl>cap)return false;body=sl+il+pl;if(body&&wire_recv_all(b->fd,frame+sizeof(*h),body)!=(ssize_t)body)return false;off=sizeof(*h)+sl+il;memset(src,0,sizeof(*src));memset(dst,0,sizeof(*dst));src->afi=dst->afi=(eigrp_afi_t)h->afi;memcpy(src->bytes,h->source,16);memcpy(dst->bytes,h->destination,16);*ifindex=ntohl(h->ifindex);memcpy(buffer,frame+off,pl);*rlen=pl;meta->ingress_vrf_id=eigrp_instance_vrf_id(e);meta->network_header_length=0;meta->eigrp_length=(uint16_t)pl;meta->destination_multicast=(h->flags&EIGRP_UNIX_WIRE_FLAG_MULTICAST)!=0;return true; }
int eigrp_sys_ipv4_packet_send(eigrp_instance_t*e,eigrp_intf_t*i,const eigrp_address_t*d,const uint8_t*p,size_t l){return eigrp_sys_packet_send(e,i,d,p,l);} bool eigrp_sys_ipv4_packet_receive(eigrp_instance_t*e,uint8_t*b,size_t c,size_t*l,eigrp_ifindex_t*i,eigrp_address_t*s,eigrp_address_t*d,eigrp_packet_rx_meta_t*m){return eigrp_sys_packet_receive(e,b,c,l,i,s,d,m);} int eigrp_sys_ipv6_packet_send(eigrp_instance_t*e,eigrp_intf_t*i,const eigrp_address_t*d,const uint8_t*p,size_t l){return eigrp_sys_packet_send(e,i,d,p,l);} bool eigrp_sys_ipv6_packet_receive(eigrp_instance_t*e,uint8_t*b,size_t c,size_t*l,eigrp_ifindex_t*i,eigrp_address_t*s,eigrp_address_t*d,eigrp_packet_rx_meta_t*m){return eigrp_sys_packet_receive(e,b,c,l,i,s,d,m);}
