/*
 * eigrp_routemap.h
 *
 *  Created on: Nov 19, 2015
 *      Author: root
 */

#ifndef EIGRPD_EIGRP_ROUTEMAP_H_
#define EIGRPD_EIGRP_ROUTEMAP_H_

extern bool eigrp_routemap_prefix_apply(eigrp_instance_t *eigrp,
					eigrp_interface_t *ei, int in,
					struct prefix *prefix);
extern void eigrp_routemap_update(const char *);
extern void eigrp_routemap_update_redistribute(void);

extern void eigrp_route_map_init();
extern void eigrp_intf_rmap_update(struct if_rmap *);
extern void eigrp_intf_rmap_update_interface(const char *interface_name);
extern void eigrp_rmap_update(const char *);

#endif /* EIGRPD_EIGRP_ROUTEMAP_H_ */
