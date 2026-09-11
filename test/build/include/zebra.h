#ifndef EIGRP_TEST_ZEBRA_H
#define EIGRP_TEST_ZEBRA_H
#define _GNU_SOURCE 1
#include <assert.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define __packed __attribute__((packed))
#define frr_weak __attribute__((weak))
#define PRINTFRR(a,b)
#define XREF_INIT(a,b,c) NULL
#define XREF_LINK(x) do { (void)(x); } while (0)
#define array_size(x) (sizeof(x) / sizeof((x)[0]))
#define CHECK_FLAG(V,F) (((V) & (F)) != 0)
#define SET_FLAG(V,F) ((V) |= (F))
#define UNSET_FLAG(V,F) ((V) &= ~(F))
#define IPV4_MAX_BYTELEN 4
#define IPV4_MAX_BITLEN 32
#define IPV6_MAX_BYTELEN 16
#define PREFIX_STRLEN 128
#define VRF_NAMSIZ 64
#define IFNAMSIZ 64
#define VRF_DEFAULT 0
#define VRF_DEFAULT_NAME "default"
#define ROUTER_STR "Enable a routing process\n"
#define EIGRP_STR "EIGRP information\n"
#define AS_STR "Autonomous system number\n"
#define VRF_CMD_HELP_STR "VRF name\n"
#define NO_STR "Negate a command or set its defaults\n"
#define SHOW_STR "Show running system information\n"
#define CLEAR_STR "Reset functions\n"
#define DEBUG_STR "Debugging functions\n"
#define UNDEBUG_STR "Disable debugging functions\n"
#define IFNAME_STR "Interface name\n"
#define IP_STR "IP information\n"
#define FRR_REDIST_STR_EIGRPD "<connected|static|kernel>"
#define REDIST_STR "Redistribute information from another routing protocol\n"
#define FRR_REDIST_HELP_STR_EIGRPD "Connected\nStatic\nKernel\n"
#define VTY_NEWLINE "\n"
#define VTY_CURR_XPATH ((vty && vty->xpath_index > 0) ? vty->xpath[vty->xpath_index - 1] : "")
#define XPATH_MAXLEN 1024
#define ZEBRA_ROUTE_MAX 256
#define ZEBRA_ROUTE_EIGRP 88
#define ZEBRA_ROUTE_CONNECT 1
#define ZEBRA_ROUTE_STATIC 2
#define ZEBRA_ROUTE_KERNEL 3
#define ZEBRA_ROUTE_MAX 256
#define DPLANE_OP_ROUTE_INSTALL 1
#define DPLANE_OP_ROUTE_UPDATE 2
#define DPLANE_OP_ROUTE_DELETE 3
#define DPLANE_OP_NONE 0
#define DPLANE_OP_ROUTE_NOTIFY 4
#define DPLANE_OP_ROUTE_REMOVE 5
#define DPLANE_OP_ADDR_INSTALL 6
#define DPLANE_OP_ADDR_UNINSTALL 7
#define ZEBRA_FLAG_SELECTED 0x1
#define ZEBRA_FLAG_INTERNAL 0x2
#define RIB_SYSTEM_ROUTE 0x1
#define AFI_IP 1
#define AFI_IP6 2
#define SAFI_UNICAST 1
#define VTY_TERM 0
#define VTY_FILE 1
#define VTY_SHELL 2
#define LOG_INFO 6
#define LOG_DEBUG 7
#define CMD_ATTR_HIDDEN 0x01
#define CMD_ATTR_YANG 0x02
#define CMD_ATTR_NOSH 0x04
#define CMD_ATTR_DEPRECATED 0x08
#define XREFT_DEFUN 0
#define QOBJ_FIELDS int qobj_dummy
#define DECLARE_QOBJ_TYPE(x)
#define DEFINE_QOBJ_TYPE(x)
#define QOBJ_REG(obj, type) do { (void)(obj); } while (0)
#define DECLARE_MGROUP(x)
#define DEFINE_MGROUP(a,b)
#define DEFINE_MTYPE_STATIC(a,b,c)
#define DEFINE_MTYPE(a,b,c)
#define DEFINE_HOOK(a,b,c)
#define FRR_DAEMON_INFO(...)
#define FRR_DAEMON_INFO_VTY_PORT(...)
#define FRR_NORETURN
#define FRR_UNUSED __attribute__((unused))

typedef unsigned int vrf_id_t;
typedef uint32_t ifindex_t;
typedef unsigned int route_tag_t;
typedef uint64_t route_value_t;

enum node_type { VIEW_NODE, ENABLE_NODE, CONFIG_NODE, INTERFACE_NODE, EIGRP_NODE, DEBUG_NODE };
struct candidate_config_stub { struct lyd_node *dnode; };
struct vty { int type; int node; int xpath_index; char xpath[8][XPATH_MAXLEN]; void *index; void *index_sub; struct candidate_config_stub *candidate_config; };
struct cmd_token { const char *text; const char *arg; const char *varname; int type; };
struct cmd_element { const char *string; int (*func)(const struct cmd_element *, struct vty *, int, struct cmd_token *[]); const char *doc; int attr; int daemon; const char *name; void *xref; };
struct cmd_node { const char *name; enum node_type node; enum node_type parent_node; const char *prompt; int (*config_write)(struct vty *); int (*node_exit)(struct vty *); void *cmdgraph; void *cmd_vector; void *cmd_hash; bool graph_built; bool no_xpath; };
struct listnode { struct listnode *next; void *data; };
struct list { struct listnode *head; struct listnode *tail; unsigned int count; void (*del)(void *); int (*cmp)(void *, void *); };
struct prefix { uint8_t family; uint8_t prefixlen; union { struct in_addr prefix4; struct in6_addr prefix6; } u; };
struct prefix_ipv4 { uint8_t family; uint8_t prefixlen; struct in_addr prefix; };
union sockunion { struct sockaddr sa; struct sockaddr_in sin; struct sockaddr_in6 sin6; };
struct route_table { struct route_node *top; };
struct route_node { struct route_node *next; struct route_node *parent; struct route_table *table; struct prefix p; void *info; unsigned int lock; };
struct event { int dummy; };
struct event_loop { int dummy; };
struct stream { unsigned char *data; size_t size; size_t endp; size_t getp; };
struct if_stats { uint64_t rx_packets; uint64_t tx_packets; };
struct if_data { uint32_t ifi_metric; uint32_t ifi_mtu; };
struct interface { char name[IFNAMSIZ]; ifindex_t ifindex; unsigned int flags; unsigned int status; int operative; void *info; struct list *connected; uint32_t bandwidth; uint32_t mtu; vrf_id_t vrf_id; struct vrf *vrf; struct if_data *metric; struct if_stats stats; };
struct vrf { char name[VRF_NAMSIZ]; vrf_id_t vrf_id; };
struct connected { struct interface *ifp; struct prefix *address; struct prefix *destination; unsigned int flags; };
struct access_list { int dummy; };
struct prefix_list { int dummy; };
struct route_map { int dummy; };
struct if_rmap { char *ifname; char *routemap[2]; };
struct distribute { char *ifname; char *list[4]; char *prefix[4]; };
struct distribute_ctx { struct vrf *vrf; };
struct lysc_node { const char *name; };
struct lyd_node { const struct lysc_node *schema; int dummy; };
struct nb_resource { void *ptr; };
struct nb_cb_create_args { const char *xpath; const struct lyd_node *dnode; int event; struct nb_resource *resource; char *errmsg; size_t errmsg_len; };
struct nb_cb_modify_args { const char *xpath; const struct lyd_node *dnode; int event; const char *value; struct nb_resource *resource; char *errmsg; size_t errmsg_len; };
struct nb_cb_destroy_args { const char *xpath; const struct lyd_node *dnode; int event; struct nb_resource *resource; char *errmsg; size_t errmsg_len; };
struct nb_cb_apply_finish_args { const char *xpath; const struct lyd_node *dnode; int event; struct nb_resource *resource; char *errmsg; size_t errmsg_len; };
struct nb_callbacks { int (*create)(); int (*modify)(); int (*destroy)(); unsigned int flags; void (*apply_finish)(); void (*cli_show)(struct vty *, const struct lyd_node *, bool); void (*cli_show_end)(struct vty *, const struct lyd_node *); };
#define F_NB_CB_DESTROY_RECURSE 0x01
struct nb_node { const char *xpath; struct nb_callbacks cbs; };
struct frr_yang_module_info { const char *name; struct nb_node nodes[192]; };
typedef int zebra_capabilities_t;
struct zebra_privs_t { const char *user; const char *group; const char *vty_group; zebra_capabilities_t *caps_p; size_t cap_num_p; size_t cap_num_i; };
struct zclient;
typedef int zclient_handler();
union g_addr { struct in_addr ipv4; struct in6_addr ipv6; };
struct zclient { void (*zebra_connected)(struct zclient *); void *context; int sock; struct stream *ibuf; int redist[3][ZEBRA_ROUTE_MAX + 1]; int default_information[3]; };
struct zapi_nexthop { int type; vrf_id_t vrf_id; ifindex_t ifindex; union g_addr gate; };
struct zapi_route { int type; int safi; int vrf_id; struct prefix prefix; uint8_t distance; uint32_t metric; uint32_t mtu; uint32_t tag; uint32_t message; struct zapi_nexthop nexthops[8]; int nexthop_num; };
struct zebra_dplane_ctx { int dummy; };
struct nexthop { int dummy; };
struct bfd_session_params { int dummy; };
struct keychain { int dummy; };
struct key { int index; char *string; };
struct message { int key; const char *str; };
typedef struct { uint32_t a[4]; } MD5_CTX;
typedef struct { uint32_t a[8]; } HMAC_SHA256_CTX;
static inline void MD5Init(MD5_CTX *ctx) { (void)ctx; }
static inline void MD5Update(MD5_CTX *ctx, const void *buf, size_t len) { (void)ctx; (void)buf; (void)len; }
static inline void MD5Final(unsigned char *digest, MD5_CTX *ctx) { (void)ctx; if (digest) memset(digest, 0, 16); }
static inline void HMAC__SHA256_Init(HMAC_SHA256_CTX *ctx, const void *key, size_t len) { (void)ctx; (void)key; (void)len; }
static inline void HMAC__SHA256_Update(HMAC_SHA256_CTX *ctx, const void *buf, size_t len) { (void)ctx; (void)buf; (void)len; }
static inline void HMAC__SHA256_Final(unsigned char *digest, HMAC_SHA256_CTX *ctx) { (void)ctx; if (digest) memset(digest, 0, 32); }
static inline struct keychain *keychain_lookup(const char *name) { (void)name; static struct keychain kc; return &kc; }
static inline struct key *key_lookup_for_send(struct keychain *kc) { (void)kc; static struct key k = { .index = 1, .string = "stub" }; return &k; }

#define ALL_LIST_ELEMENTS(list,node,nnode,datum) \
	(node) = ((list) ? (list)->head : NULL); \
	(node) && (((nnode) = (node)->next), ((datum) = (node)->data), 1); \
	(node) = (nnode)
#define ALL_LIST_ELEMENTS_RO(list,node,datum) \
	(node) = ((list) ? (list)->head : NULL); \
	(node) && (((datum) = (node)->data), 1); \
	(node) = (node)->next

#define listhead(L) ((L) ? (L)->head : NULL)
#define listnextnode(N) ((N) ? (N)->next : NULL)
#define listgetdata(N) ((N) ? (N)->data : NULL)
#define listcount(L) ((L) ? (L)->count : 0)

static inline struct list *list_new(void) { return calloc(1, sizeof(struct list)); }
static inline void list_delete(struct list **l) { if (l) { free(*l); *l = NULL; } }
static inline void listnode_add(struct list *l, void *data) { if (!l) return; struct listnode *n = calloc(1, sizeof(*n)); n->data=data; if (!l->head) l->head=n; else l->tail->next=n; l->tail=n; l->count++; }
static inline void listnode_delete(struct list *l, void *data) { (void)l; (void)data; }
static inline struct route_table *route_table_init(void) { return calloc(1, sizeof(struct route_table)); }
static inline void route_table_finish(struct route_table *t) { free(t); }
static inline struct route_node *route_node_get(struct route_table *t, const struct prefix *p) { (void)p; struct route_node *n=calloc(1,sizeof(*n)); n->table=t; if (p) n->p=*p; return n; }

static inline struct route_node *route_node_match(struct route_table *t, const struct prefix *p) { (void)t; (void)p; return NULL; }
static inline const char *ifindex2ifname(ifindex_t ifindex, vrf_id_t vrf_id) { (void)ifindex; (void)vrf_id; return "stub0"; }
static inline void vty_time_print(struct vty *vty, int seconds) { (void)vty; (void)seconds; }

static inline struct route_node *route_node_lookup(struct route_table *t, const struct prefix *p) { (void)t; (void)p; return NULL; }
static inline struct route_node *route_top(struct route_table *t) { return t ? t->top : NULL; }
static inline struct route_node *route_next(struct route_node *n) { return n ? n->next : NULL; }
static inline void route_unlock_node(struct route_node *n) { (void)n; }
static inline void route_lock_node(struct route_node *n) { (void)n; }

static inline struct stream *stream_new(size_t size) { struct stream *s=calloc(1,sizeof(*s)); s->data=calloc(1,size); s->size=size; return s; }
static inline void stream_free(struct stream *s) { if (s) { free(s->data); free(s); } }
static inline void stream_reset(struct stream *s) { if (s) s->endp=s->getp=0; }
static inline size_t stream_put(struct stream *s, const void *buf, size_t len) { if (!s || s->endp + len > s->size) return 0; memcpy(s->data+s->endp, buf, len); s->endp += len; return len; }
static inline size_t stream_putc(struct stream *s, uint8_t v) { return stream_put(s,&v,1); }
static inline size_t stream_putw(struct stream *s, uint16_t v) { uint16_t n=htons(v); return stream_put(s,&n,2); }
static inline size_t stream_putl(struct stream *s, uint32_t v) { uint32_t n=htonl(v); return stream_put(s,&n,4); }
static inline size_t stream_put_ipv4(struct stream *s, uint32_t v) { return stream_put(s,&v,4); }
static inline void stream_get(void *dst, struct stream *s, size_t len) { if (!dst || !s) return; if (s->getp + len > s->endp) len = (s->endp > s->getp) ? s->endp - s->getp : 0; memcpy(dst, s->data + s->getp, len); s->getp += len; }
static inline uint8_t stream_getc(struct stream *s) { return (s && s->getp < s->endp) ? s->data[s->getp++] : 0; }
static inline uint16_t stream_getw(struct stream *s) { uint16_t v=0; if (s && s->getp+2<=s->endp) { memcpy(&v,s->data+s->getp,2); s->getp+=2; } return ntohs(v); }
static inline uint32_t stream_getl(struct stream *s) { uint32_t v=0; if (s && s->getp+4<=s->endp) { memcpy(&v,s->data+s->getp,4); s->getp+=4; } return ntohl(v); }
static inline uint32_t stream_get_ipv4(struct stream *s) { uint32_t v=0; if (s && s->getp+4<=s->endp) { memcpy(&v,s->data+s->getp,4); s->getp+=4; } return v; }
static inline size_t stream_get_endp(const struct stream *s) { return s ? s->endp : 0; }
static inline void stream_set_endp(struct stream *s, size_t p) { if (s) s->endp=p; }
static inline size_t stream_get_getp(const struct stream *s) { return s ? s->getp : 0; }
static inline void stream_set_getp(struct stream *s, size_t p) { if (s) s->getp=p; }
static inline unsigned char *STREAM_DATA(struct stream *s) { return s ? s->data : NULL; }
static inline size_t STREAM_WRITEABLE(struct stream *s) { return s && s->size>=s->endp ? s->size - s->endp : 0; }

static inline int vty_out(struct vty *vty, const char *fmt, ...) { (void)vty; (void)fmt; return 0; }
static inline int vty_out_newline(struct vty *vty, const char *fmt, ...) { (void)vty; (void)fmt; return 0; }

struct ip { unsigned int ip_hl:4, ip_v:4; uint8_t ip_tos; uint16_t ip_len; uint16_t ip_id; uint16_t ip_off; uint8_t ip_ttl; uint8_t ip_p; uint16_t ip_sum; struct in_addr ip_src; struct in_addr ip_dst; };
static inline int event_timer_remain_second(struct event *e) { (void)e; return 0; }
static inline const char *lookup_msg(const struct message *msg, int key, const char *def) { (void)msg; (void)key; return def ? def : "unknown"; }
static inline int argv_find(struct cmd_token **argv, int argc, const char *token, int *idx) { (void)argv; (void)argc; (void)token; if (idx) *idx = 0; return 0; }

static inline void zlog_debug(const char *fmt, ...) { (void)fmt; }
static inline void zlog_warn(const char *fmt, ...) { (void)fmt; }
static inline void zlog_err(const char *fmt, ...) { (void)fmt; }
static inline void flog_err(int code, const char *fmt, ...) { (void)code; (void)fmt; }
static inline void install_element(enum node_type node, const struct cmd_element *cmd) { (void)node; (void)cmd; }
static inline void install_node(struct cmd_node *node) { (void)node; }
static inline void install_default(enum node_type node) { (void)node; }
static inline void if_cmd_init_default(void) {}
static inline void vrf_cmd_init(void *x) { (void)x; }
static inline void VTY_PUSH_XPATH(enum node_type node, const char *xpath) { (void)node; (void)xpath; }
static inline int nb_cli_enqueue_change(struct vty *vty, const char *xpath, int op, const char *value) { (void)vty;(void)xpath;(void)op;(void)value; return 0; }
static inline int nb_cli_apply_changes(struct vty *vty, void *x) { (void)vty;(void)x; return 0; }
static inline int nb_cli_apply_changes_clear_pending(struct vty *vty, void *x) { (void)vty;(void)x; return 0; }
static inline void nb_cli_show_dnode_cmds(struct vty *vty, const struct lyd_node *dnode, bool defaults) { (void)vty;(void)dnode;(void)defaults; }
static inline const char *yang_dnode_get_string(const struct lyd_node *dnode, const char *xpath) { (void)dnode;(void)xpath; return "0"; }
static inline bool yang_dnode_exists(const struct lyd_node *dnode, const char *xpath) { (void)dnode;(void)xpath; return false; }
static inline const struct lyd_node *yang_dnode_get(const struct lyd_node *dnode, const char *xpath) { (void)dnode;(void)xpath; return NULL; }
static inline const char *lyd_get_value(const struct lyd_node *dnode) { (void)dnode; return ""; }
static inline const struct lyd_node *lyd_parent(const struct lyd_node *dnode) { return dnode; }
static inline const struct lyd_node *yang_dnode_getf(const struct lyd_node *dnode, const char *fmt, ...) { (void)dnode;(void)fmt; return NULL; }
static inline int str2prefix_ipv4(const char *s, struct prefix_ipv4 *p) { (void)s; if (p) { memset(p,0,sizeof(*p)); p->family=AF_INET; } return 1; }
static inline int str2prefix(const char *s, struct prefix *p) { (void)s; if (p) memset(p,0,sizeof(*p)); return 1; }
static inline const char *prefix2str(const struct prefix *p, char *buf, size_t len) { (void)p; if (buf && len) snprintf(buf,len,"0.0.0.0/0"); return buf; }
static inline struct vrf *vrf_lookup_by_name(const char *name) { static struct vrf v; snprintf(v.name,sizeof(v.name),"%s",name?name:VRF_DEFAULT_NAME); v.vrf_id=VRF_DEFAULT; return &v; }
static inline struct vrf *vrf_lookup_by_id(vrf_id_t id) { static struct vrf v; snprintf(v.name,sizeof(v.name),"%s",VRF_DEFAULT_NAME); v.vrf_id=id; return &v; }
static inline struct interface *if_lookup_by_name_args(const char *name, vrf_id_t vrf_id, ...) { static struct interface ifp; (void)vrf_id; snprintf(ifp.name,sizeof(ifp.name),"%s",name?name:"stub0"); return &ifp; }
#define if_lookup_by_name(...) if_lookup_by_name_args(__VA_ARGS__, VRF_DEFAULT)
static inline struct interface *if_lookup_by_index(ifindex_t ifindex, vrf_id_t vrf_id) { static struct interface ifp; (void)ifindex;(void)vrf_id; snprintf(ifp.name,sizeof(ifp.name),"stub0"); return &ifp; }
static inline void if_add_hook(int type, int (*fn)(struct interface *)) { (void)type;(void)fn; }
static inline void if_del_hook(int type, int (*fn)(struct interface *)) { (void)type;(void)fn; }
static inline void *XCALLOC(int type, size_t size) { (void)type; return calloc(1,size); }
static inline void *XMALLOC(int type, size_t size) { (void)type; return malloc(size); }
static inline void XFREE(int type, void *ptr) { (void)type; free(ptr); }
static inline char *XSTRDUP(int type, const char *s) { (void)type; return s ? strdup(s) : NULL; }

#define CMD_SUCCESS 0
#define CMD_WARNING 1
#define CMD_WARNING_CONFIG_FAILED 13
#define CMD_ERR_NO_MATCH 2
#define CMD_ERR_NOTHING_TODO 6
#define NB_OP_CREATE 1
#define NB_OP_DESTROY 2
#define NB_OP_MODIFY 3
#define NB_OP_DELETE 4
#define NB_EV_VALIDATE 1
#define NB_EV_PREPARE 2
#define NB_EV_ABORT 3
#define NB_EV_APPLY 4
#define EIGRP_FERR_START 10000
#define END_FERR 0
struct log_ref { int code; const char *title; const char *description; const char *suggestion; };
static inline void log_ref_add(struct log_ref *refs) { (void)refs; }
#define MTYPE_EIGRP_AUTH_TLV 1001
#define MTYPE_EIGRP_AUTH_SHA256_TLV 1002
#define MTYPE_EIGRP_TOP_ENTRY 1003
#define MTYPE_EIGRP_TOP 1004
#define MTYPE_EIGRP_NEIGHBOR 1005
#define MTYPE_EIGRP_INTERFACE 1006
#define MTYPE_EIGRP_PACKET 1007
#define MTYPE_EIGRP_PACKET_QUEUE 1008
#define MTYPE_EIGRP_FIFO 1009
#define MTYPE_EIGRP 1010
#define WORD_TKN 1

#ifndef DEFUN_CMD_ELEMENT
#define DEFUN_CMD_ELEMENT(funcname, cmdname, cmdstr, helpstr, attrs, dnum) \
	const struct cmd_element cmdname = { .string = cmdstr, .func = funcname, .doc = helpstr, .attr = attrs, .daemon = dnum, .name = #cmdname, .xref = NULL };
#define DEFUN_CMD_FUNC_DECL(funcname) static int funcname(const struct cmd_element *, struct vty *, int, struct cmd_token *[])
#define DEFUN_CMD_FUNC_TEXT(funcname) static int funcname(const struct cmd_element *self __attribute__((unused)), struct vty *vty __attribute__((unused)), int argc __attribute__((unused)), struct cmd_token *argv[] __attribute__((unused)))
#define DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, attr) DEFUN_CMD_FUNC_DECL(funcname); static DEFUN_CMD_ELEMENT(funcname, cmdname, cmdstr, helpstr, attr, 0) DEFUN_CMD_FUNC_TEXT(funcname)
#define DEFUN(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, 0)
#define DEFUN_HIDDEN(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_HIDDEN)
#define DEFUN_NOSH(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_NOSH)
#define DEFUN_YANG(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_YANG)
#define DEFPY(funcname, cmdname, cmdstr, helpstr) DEFUN(funcname, cmdname, cmdstr, helpstr)
#define DEFPY_NOSH(funcname, cmdname, cmdstr, helpstr) DEFUN_NOSH(funcname, cmdname, cmdstr, helpstr)
#define DEFPY_YANG(funcname, cmdname, cmdstr, helpstr) DEFUN_YANG(funcname, cmdname, cmdstr, helpstr)
#define DEFPY_YANG_NOSH(funcname, cmdname, cmdstr, helpstr) DEFUN_ATTR(funcname, cmdname, cmdstr, helpstr, CMD_ATTR_YANG|CMD_ATTR_NOSH)
#define DEFPY_HIDDEN(funcname, cmdname, cmdstr, helpstr) DEFUN_HIDDEN(funcname, cmdname, cmdstr, helpstr)
#define ALIAS(funcname, cmdname, cmdstr, helpstr) static DEFUN_CMD_ELEMENT(funcname, cmdname, cmdstr, helpstr, 0, 0)
#endif

extern struct running_config_stub { struct lyd_node *dnode; } *running_config;


/* Standalone FRR surface needed by the syntax-only eigrpd build. */
#define VERSION "standalone-build"
#define NB_OK 0
#define NB_ERR_INCONSISTENCY 1
#define NB_ERR_RESOURCE 2
#define NB_ERR_VALIDATION 3
#define EC_LIB_SOCKET 20001
#define EC_LIB_DEVELOPMENT 20002
#define ZEBRA_IFA_SECONDARY 0x1
#define FILTER_DENY 1
#define PREFIX_DENY 1
#define RMAP_EIGRP 1
#define IF_RMAP_IN 0
#define IF_RMAP_OUT 1
#define MTYPE_ROUTE_MAP_COMPILED 3001
#define MTYPE_EIGRP_INTF 1011
#define MTYPE_EIGRP_INTF_INFO 1012
#define MTYPE_EIGRP_ROUTE_DESCRIPTOR 1013
#define MTYPE_EIGRP_PREFIX_DESCRIPTOR 1014
#define MTYPE_EIGRP_WORK_QUEUE 1015
#define MTYPE_EIGRP_WORK_QUEUE_NAME 1016
#define MTYPE_EIGRP_IPV4_INT_TLV 1017
#define MTYPE_EIGRP_SEQ_TLV 1018
#define MTYPE_EIGRP_PACKETIZER_WORK 1019
#define DISTRIBUTE_V4_IN 0
#define DISTRIBUTE_V4_OUT 1
#define ZCAP_NET_RAW 1
#define ZCAP_BIND 2
#define ZCAP_NET_ADMIN 3
#define MULTIPATH_NUM 8
#define ZAPI_MESSAGE_NEXTHOP 0x01
#define ZAPI_MESSAGE_METRIC 0x02
#define ZEBRA_ROUTE_ADD 1
#define ZEBRA_ROUTE_DELETE 2
#define ZEBRA_REDISTRIBUTE_ADD 3
#define ZEBRA_REDISTRIBUTE_DELETE 4
#define ZEBRA_REDISTRIBUTE_ROUTE_ADD 5
#define ZEBRA_REDISTRIBUTE_ROUTE_DEL 6
#define ZEBRA_ROUTER_ID_UPDATE 7
#define ZEBRA_INTERFACE_ADDRESS_ADD 8
#define ZEBRA_INTERFACE_ADDRESS_DELETE 9
#define ZEBRA_ROUTE_NOTIFY_OWNER 10
#define NEXTHOP_TYPE_IPV4_IFINDEX 1
#define NEXTHOP_TYPE_IFINDEX 2
#define IPVERSION 4
#define IPTOS_PREC_INTERNETCONTROL 0xc0
#define IPV4_NET127(a) (((a) & 0xff000000U) == 0x7f000000U)
#define EVENT_ARG(e) ((void *)(e))
#define RB_FOREACH(var, head, tree) for ((var) = NULL; (var) != NULL; (var) = NULL)
#define FOR_ALL_INTERFACES(vrf, ifp) for ((ifp) = NULL; (ifp) != NULL; (ifp) = NULL)
#define frr_each(type, list, item) for (struct listnode *_frr_each_node = listhead(list); _frr_each_node && (((item) = listgetdata(_frr_each_node)), 1); _frr_each_node = listnextnode(_frr_each_node))
#define frr_with_privs(privs) for (int _frr_privs_once = ((void)(privs), 1); _frr_privs_once; _frr_privs_once = 0)
#define ZAPI_CALLBACK_ARGS struct zclient *zclient, int cmd, vrf_id_t vrf_id

enum zapi_route_notify_owner { ZAPI_ROUTE_NOTIFY_OWNER_UNKNOWN = 0 };
enum route_map_cmd_result_t { RMAP_MATCH = 1, RMAP_NOMATCH = 0, RMAP_NOOP = 2, RMAP_OKAY = 3, RMAP_ERROR = 4 };
typedef int route_map_object_t;
typedef enum { RMAP_EVENT_MATCH_ADDED = 1, RMAP_EVENT_MATCH_DELETED = 2 } route_map_event_t;
enum rmap_compile_rets { RMAP_COMPILE_SUCCESS = 0, RMAP_RULE_MISSING = 1, RMAP_COMPILE_ERROR = 2 };
struct route_map_index { int dummy; };
typedef int wq_item_status;
struct work_queue;
struct work_queue_spec { void *data; wq_item_status (*workfunc)(struct work_queue *, void *); void (*del_item_data)(struct work_queue *, void *); };
struct work_queue { struct work_queue_spec spec; const char *name; int max_retries; void *thread; };
#define WQ_SUCCESS 0
#define WQ_REQUEUE 1
#define WQ_QUEUE_BLOCKED 2
struct route_map_rule_cmd { const char *str; enum route_map_cmd_result_t (*func_apply)(void *, struct prefix *, route_map_object_t, void *); void *(*func_compile)(const char *); void (*func_free)(void *); };
#ifndef EIGRP_TEST_STRUCT_OPTION
#define EIGRP_TEST_STRUCT_OPTION
struct option { const char *name; int has_arg; int *flag; int val; };
#ifndef no_argument
#define no_argument 0
#endif
#ifndef required_argument
#define required_argument 1
#endif
#ifndef optional_argument
#define optional_argument 2
#endif
#endif
struct frr_signal_t { int signal; void (*handler)(void); };
struct frr_daemon_info { const char *config_file; const char *proghelp; };
static const int config_default = 0;
extern const struct frr_yang_module_info frr_eigrpd_info;
extern const struct frr_yang_module_info frr_filter_info;
extern const struct frr_yang_module_info frr_interface_info;
extern const struct frr_yang_module_info frr_route_map_info;
extern const struct frr_yang_module_info frr_vrf_info;
extern const struct frr_yang_module_info ietf_key_chain_info;
extern const struct frr_yang_module_info ietf_key_chain_deviation_info;
extern const void *zclient_options_default;
extern struct list *iflist;

static inline void *listnode_head(struct list *l) { return l && l->head ? l->head->data : NULL; }
static inline struct listnode *listtail(struct list *l) { return l ? l->tail : NULL; }
static inline int list_isempty(const struct list *l) { return !l || !l->head; }
static inline struct listnode *listnode_lookup(struct list *l, void *data) { (void)data; return l ? l->head : NULL; }
static inline void listnode_add_sort(struct list *l, void *data) { listnode_add(l, data); }
static inline void list_delete_node(struct list *l, struct listnode *n) { (void)l; (void)n; }
static inline void list_delete_all_node(struct list *l) { if (l) { l->head = l->tail = NULL; l->count = 0; } }

static inline void event_cancel(struct event **e) { if (e) *e = NULL; }
static inline void event_cancel_event(struct event_loop *m, void *arg) { (void)m; (void)arg; }
static inline void event_add_timer(struct event_loop *m, void (*fn)(struct event *), void *arg, long t, struct event **e) { (void)m; (void)fn; (void)arg; (void)t; if (e) *e = (struct event *)arg; }
static inline void event_add_timer_msec(struct event_loop *m, void (*fn)(struct event *), void *arg, long t, struct event **e) { event_add_timer(m, fn, arg, t, e); }
static inline void event_add_write(struct event_loop *m, void (*fn)(struct event *), void *arg, int fd, struct event **e) { (void)m; (void)fn; (void)fd; if (e) *e = (struct event *)arg; }
static inline void event_add_read(struct event_loop *m, void (*fn)(struct event *), void *arg, int fd, struct event **e) { (void)m; (void)fn; (void)fd; if (e) *e = (struct event *)arg; }
static inline void event_add_event(struct event_loop *m, void (*fn)(struct event *), void *arg, int val, struct event **e) { (void)m; (void)fn; (void)val; if (e) *e = (struct event *)arg; }
static inline void event_execute(struct event_loop *m, void (*fn)(struct event *), void *arg, int val, struct event **e) { (void)m; (void)val; if (e) *e = (struct event *)arg; if (fn) fn((struct event *)arg); }
static inline void monotime(struct timeval *tv) { if (tv) gettimeofday(tv, NULL); }

static inline void zlog_info(const char *fmt, ...) { (void)fmt; }
static inline void zlog_notice(const char *fmt, ...) { (void)fmt; }
static inline void flog_err_sys(int code, const char *fmt, ...) { (void)code; (void)fmt; }
static inline const char *safe_strerror(int errnum) { return strerror(errnum); }

static inline struct prefix *prefix_new(void) { return calloc(1, sizeof(struct prefix)); }
static inline struct prefix_ipv4 *prefix_ipv4_new(void) { return calloc(1, sizeof(struct prefix_ipv4)); }
static inline void prefix_free(void *p) { free(p); }
static inline void prefix_ipv4_free(void *p) { free(p); }
static inline void prefix_copy(struct prefix *dst, const struct prefix *src) { if (dst && src) *dst = *src; }
static inline void apply_mask(struct prefix *p) { (void)p; }
static inline int prefix_cmp(const struct prefix *a, const struct prefix *b) { return memcmp(a, b, sizeof(*a)); }
static inline int prefix_match_network_statement(const struct prefix *net, const struct prefix *p) { (void)net; (void)p; return 1; }
static inline int IPV4_ADDR_SAME(const struct in_addr *a, const struct in_addr *b) { return a && b && a->s_addr == b->s_addr; }

static inline int if_is_pointopoint(const struct interface *ifp) { (void)ifp; return 0; }
static inline int if_is_loopback(const struct interface *ifp) { (void)ifp; return 0; }
static inline int if_is_up(const struct interface *ifp) { (void)ifp; return 1; }
static inline int if_is_operative(const struct interface *ifp) { return if_is_up(ifp); }
static inline void hook_register_prio(void *hook, int prio, int (*fn)(struct interface *)) { (void)hook; (void)prio; (void)fn; }
static void *if_real, *if_up, *if_down, *if_unreal, *if_del, *if_connected;

static inline struct access_list *access_list_lookup(int afi, const char *name) { (void)afi; (void)name; return NULL; }
static inline int access_list_apply(struct access_list *acl, const struct prefix *p) { (void)acl; (void)p; return 0; }
static inline void access_list_init(void) {}
static inline void access_list_add_hook(void (*fn)(struct access_list *)) { (void)fn; }
static inline void access_list_delete_hook(void (*fn)(struct access_list *)) { (void)fn; }
static inline struct prefix_list *prefix_list_lookup(int afi, const char *name) { (void)afi; (void)name; return NULL; }
static inline int prefix_list_apply(struct prefix_list *pl, const struct prefix *p) { (void)pl; (void)p; return 0; }
static inline void prefix_list_init(void) {}
static inline void prefix_list_add_hook(void (*fn)(struct prefix_list *)) { (void)fn; }
static inline void prefix_list_delete_hook(void (*fn)(struct prefix_list *)) { (void)fn; }
static inline void prefix_list_reset(void) {}

static inline struct distribute_ctx *distribute_list_ctx_create(struct vrf *vrf) { struct distribute_ctx *ctx = calloc(1, sizeof(*ctx)); ctx->vrf = vrf; return ctx; }
static inline void distribute_list_add_hook(struct distribute_ctx *ctx, void (*fn)(struct distribute_ctx *, struct distribute *)) { (void)ctx; (void)fn; }
static inline void distribute_list_delete_hook(struct distribute_ctx *ctx, void (*fn)(struct distribute_ctx *, struct distribute *)) { (void)ctx; (void)fn; }
static inline void distribute_list_delete(struct distribute_ctx **ctx) { if (ctx) { free(*ctx); *ctx = NULL; } }
static inline struct distribute *distribute_lookup(struct distribute_ctx *ctx, const char *ifname) { (void)ctx; (void)ifname; return NULL; }

static inline uint32_t yang_dnode_get_uint32(const struct lyd_node *dnode, const char *xpath_fmt, ...) { (void)dnode; (void)xpath_fmt; return 0; }
static inline uint16_t yang_dnode_get_uint16(const struct lyd_node *dnode, const char *xpath_fmt, ...) { (void)dnode; (void)xpath_fmt; return 0; }
static inline uint8_t yang_dnode_get_uint8(const struct lyd_node *dnode, const char *xpath_fmt, ...) { (void)dnode; (void)xpath_fmt; return 0; }
static inline bool yang_dnode_get_bool(const struct lyd_node *dnode, const char *xpath) { (void)dnode; (void)xpath; return false; }
static inline int yang_dnode_get_ipv4(struct in_addr *addr, const struct lyd_node *dnode, const char *xpath) { (void)dnode; (void)xpath; if (addr) addr->s_addr = 0; return 0; }
static inline int yang_dnode_get_ipv4p(struct prefix *p, const struct lyd_node *dnode, const char *xpath) { (void)dnode; (void)xpath; if (p) memset(p, 0, sizeof(*p)); return 0; }
static inline int yang_dnode_get_enum(const struct lyd_node *dnode, const char *xpath) { (void)dnode; (void)xpath; return 0; }
static inline const char *yang_dnode_get_relative_path(const struct lyd_node *dnode, const char *xpath) { (void)dnode; (void)xpath; return ""; }
static inline int nb_running_set_entry(const struct lyd_node *dnode, void *entry) { (void)dnode; (void)entry; return NB_OK; }
static inline void *nb_running_unset_entry(const struct lyd_node *dnode) { (void)dnode; return NULL; }
static inline void *nb_running_get_entry(const struct lyd_node *dnode, const char *xpath, bool abort_if_null) { (void)dnode; (void)xpath; (void)abort_if_null; return NULL; }

static inline void keychain_init(void) {}
static inline void keychain_terminate(void) {}
static inline void route_map_init(void) {}
static inline void route_map_init_vty(void) {}
static inline void route_map_finish(void) {}
static inline int str2sockunion(const char *str, union sockunion *su) { if (su) memset(su, 0, sizeof(*su)); if (!str) return -1; if (su && inet_pton(AF_INET, str, &su->sin.sin_addr) == 1) { su->sin.sin_family = AF_INET; return 0; } return -1; }
static inline struct route_map *route_map_lookup_by_name(const char *name) { (void)name; return NULL; }
static inline const char *route_map_name(const struct route_map *map) { (void)map; return ""; }
static inline void route_map_add_hook(void (*fn)(const char *)) { (void)fn; }
static inline void route_map_delete_hook(void (*fn)(const char *)) { (void)fn; }
static inline enum rmap_compile_rets route_map_add_match(struct route_map_index *index, const char *rule, const char *arg, route_map_event_t event) { (void)index; (void)rule; (void)arg; (void)event; return RMAP_COMPILE_SUCCESS; }
static inline enum rmap_compile_rets route_map_delete_match(struct route_map_index *index, const char *rule, const char *arg, route_map_event_t event) { (void)index; (void)rule; (void)arg; (void)event; return RMAP_COMPILE_SUCCESS; }
static inline enum rmap_compile_rets route_map_add_set(struct route_map_index *index, const char *rule, const char *arg) { (void)index; (void)rule; (void)arg; return RMAP_COMPILE_SUCCESS; }
static inline enum rmap_compile_rets route_map_delete_set(struct route_map_index *index, const char *rule, const char *arg) { (void)index; (void)rule; (void)arg; return RMAP_COMPILE_SUCCESS; }
static inline struct if_rmap *if_rmap_lookup(const char *ifname) { (void)ifname; return NULL; }
static inline void if_rmap_init(enum node_type node) { (void)node; }

static inline int vrf_socket(int family, int type, int proto, vrf_id_t vrf_id, const char *name) { (void)family; (void)type; (void)proto; (void)vrf_id; (void)name; return 0; }
static inline void vrf_init(void *master, void *info, bool enabled, void *unused) { (void)master; (void)info; (void)enabled; (void)unused; }
static inline void vrf_terminate(void) {}
static inline int setsockopt_so_sendbuf(int fd, unsigned int size) { (void)fd; return (int)size; }
static inline int getsockopt_so_sendbuf(int fd) { (void)fd; return 0; }
static inline int setsockopt_ifindex(int family, int fd, int val) { (void)family; (void)fd; (void)val; return 0; }
static inline int setsockopt_ipv4_multicast_if(int fd, struct in_addr addr, ifindex_t ifindex) { (void)fd; (void)addr; (void)ifindex; return 0; }
static inline int setsockopt_ipv4_multicast(int fd, int opt, struct in_addr addr, uint32_t group, ifindex_t ifindex) { (void)fd; (void)opt; (void)addr; (void)group; (void)ifindex; return 0; }
static inline int setsockopt_ipv4_tos(int fd, int tos) { (void)fd; (void)tos; return 0; }

static inline void zebra_router_id_update_read(struct stream *s, struct prefix *p) { (void)s; if (p) memset(p, 0, sizeof(*p)); }
static inline int zapi_route_notify_decode(struct stream *s, struct prefix *p, uint32_t *table, enum zapi_route_notify_owner *note, void *a, void *b) { (void)s; (void)a; (void)b; if (p) memset(p, 0, sizeof(*p)); if (table) *table = 0; if (note) *note = 0; return 1; }
static inline int zapi_route_decode(struct stream *s, struct zapi_route *api) { (void)s; if (api) memset(api, 0, sizeof(*api)); return 0; }
static inline struct connected *zebra_interface_address_read(int cmd, struct stream *s, vrf_id_t vrf_id) { (void)cmd; (void)s; (void)vrf_id; return NULL; }
static inline void connected_free(struct connected **c) { if (c) { free(*c); *c = NULL; } }
static inline void zapi_route_init(struct zapi_route *api) { if (api) memset(api, 0, sizeof(*api)); }
static inline void zapi_nexthop_init(struct zapi_nexthop *nh) { if (nh) memset(nh, 0, sizeof(*nh)); }
static inline int zclient_route_send(int cmd, struct zclient *zc, struct zapi_route *api) { (void)cmd; (void)zc; (void)api; return 0; }
static inline int vrf_bitmap_check(int *bitmap, vrf_id_t vrf_id) { (void)bitmap; (void)vrf_id; return 0; }
static inline void zclient_redistribute(int cmd, struct zclient *zc, int afi, int type, int instance, vrf_id_t vrf_id) { (void)cmd; (void)zc; (void)afi; (void)type; (void)instance; (void)vrf_id; }
static inline struct zclient *zclient_new(struct event_loop *m, const void *opts, zclient_handler *const handlers[], size_t n) { (void)m; (void)opts; (void)handlers; (void)n; return calloc(1, sizeof(struct zclient)); }
static inline void zclient_init(struct zclient *zc, int route, int flags, struct zebra_privs_t *privs) { (void)zc; (void)route; (void)flags; (void)privs; }
static inline void zclient_send_reg_requests(struct zclient *zc, vrf_id_t vrf_id) { (void)zc; (void)vrf_id; }
static inline void zclient_stop(struct zclient *zc) { (void)zc; }
static inline void zclient_free(struct zclient *zc) { free(zc); }
static inline void *stream_pnt(struct stream *s) { return s ? (void *)(s->data + s->getp) : NULL; }
static inline void stream_forward_getp(struct stream *s, size_t n) { if (s) s->getp = (s->getp + n <= s->endp) ? s->getp + n : s->endp; }
static inline void stream_forward_endp(struct stream *s, size_t n) { if (s) s->endp = (s->endp + n <= s->size) ? s->endp + n : s->size; }
static inline void stream_copy(struct stream *dst, const struct stream *src) { if (!dst || !src) return; size_t n = src->endp < dst->size ? src->endp : dst->size; memcpy(dst->data, src->data, n); dst->endp = n; dst->getp = src->getp < n ? src->getp : n; }
static inline void sockopt_iphdrincl_swab_htosys(struct ip *ip) { (void)ip; }
static inline void sockopt_iphdrincl_swab_systoh(struct ip *ip) { (void)ip; }
static inline struct connected *if_lookup_address(void *addr, int family, vrf_id_t vrf_id) { (void)addr; (void)family; (void)vrf_id; return NULL; }
#define SOPT_SIZE_CMSG_IFINDEX_IPV4() ((size_t)sizeof(ifindex_t))
static inline ssize_t stream_recvmsg(struct stream *s, int fd, struct msghdr *msg, int flags, size_t size) { (void)s; (void)fd; (void)msg; (void)flags; (void)size; return 0; }
static inline ifindex_t getsockopt_ifindex(int family, struct msghdr *msg) { (void)family; (void)msg; return 0; }
static inline uint16_t in_cksum(const void *ptr, size_t len) { (void)ptr; (void)len; return 0; }
static inline void masklen2ip(uint8_t prefixlen, struct in_addr *addr) { (void)prefixlen; if (addr) addr->s_addr = 0; }

static inline struct work_queue *work_queue_new(struct event_loop *m, const char *name) { (void)m; struct work_queue *q = calloc(1, sizeof(*q)); q->name = name; return q; }
static inline void work_queue_free_and_null(struct work_queue **q) { if (q) { free(*q); *q = NULL; } }
static inline void work_queue_add(struct work_queue *q, void *data) { (void)q; (void)data; }

static inline void frr_preinit(struct frr_daemon_info *di, int argc, char **argv) { (void)di; (void)argc; (void)argv; }
static inline void frr_opt_add(const char *opts, const struct option *longopts, const char *help) { (void)opts; (void)longopts; (void)help; }
static inline int frr_getopt(int argc, char **argv, void *longindex) { (void)argc; (void)argv; (void)longindex; return EOF; }
static inline void frr_help_exit(int status) { exit(status); }
static inline struct event_loop *frr_init(void) { return calloc(1, sizeof(struct event_loop)); }
static inline void frr_fini(void) {}
static inline void frr_config_fork(void) {}
static inline void frr_run(struct event_loop *m) { (void)m; }
static inline void zlog_rotate(void) {}
static inline int vty_read_config(void *a, const char *file, int cfg) { (void)a; (void)file; (void)cfg; return 0; }
static inline void libagentx_init(void) {}

#endif
