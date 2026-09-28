#include <zebra.h>
#include <northbound.h>
#include <vty.h>

struct distribute_ctx;

extern int group_distribute_list_create_helper(struct nb_cb_create_args *args,
					       struct distribute_ctx *ctx);
extern int group_distribute_list_destroy(struct nb_cb_destroy_args *args);
extern int group_distribute_list_ipv4_modify(struct nb_cb_modify_args *args);
extern int group_distribute_list_ipv4_destroy(struct nb_cb_destroy_args *args);
extern void group_distribute_list_ipv4_cli_show(struct vty *vty,
						const struct lyd_node *dnode,
						bool show_defaults);
