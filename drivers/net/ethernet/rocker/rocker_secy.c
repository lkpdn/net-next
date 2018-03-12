/*
 * drivers/net/ethernet/rocker/rocker_pac.c - Rocker switch SecY
 *					      implementation
 * Copyright (c) 2018 lkpdn <den@klaipeden.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/if_vlan.h>
#include <linux/if_bridge.h>
#include <net/switchdev.h>

#include "rocker.h"
#include "rocker_tlv.h"

struct secy {
	struct rocker *rocker;
};

struct secy_port {
	struct secy *secy;
	struct rocker_port *rocker_port;
	struct net_device *dev;
};

/**********************************
 * Rocker world ops implementation
 **********************************/

static int secy_init(struct rocker *rocker)
{
	return 0;
}

static void secy_fini(struct rocker *rocker)
{
	return;
}

static int secy_port_pre_init(struct rocker_port *rocker_port)
{
	return 0;
}

static int secy_port_init(struct rocker_port *rocker_port)
{
	return 0;
}

static void secy_port_fini(struct rocker_port *rocker_port)
{
	return 0;
}

static int secy_port_open(struct rocker_port *rocker_port)
{
	return 0;
}

static void secy_port_stop(struct rocker_port *rocker_port)
{
	return NULL;
}

static int secy_port_attr_stp_state_set(struct rocker_port *rocker_port,
					u8 state)
{
	return 0;
}

static int secy_port_attr_bridge_flags_set(struct rocker_port *rocker_port,
					   unsigned long brport_flags,
					   struct switchdev_trans *trans)
{
	return 0;
}

static int
secy_port_attr_bridge_flags_get(const struct rocker_port *rocker_port,
				unsigned long *p_brport_flags)
{
	return 0;
}

static int
secy_port_attr_bridge_flags_support_get(const struct rocker_port *
					rocker_port,
					unsigned long *
					p_brport_flags_support)
{
	return 0;
}

static int
secy_port_attr_bridge_ageing_time_set(struct rocker_port *rocker_port,
				      u32 ageing_time,
				      struct switchdev_trans *trans)
{
	return 0;
}

static int secy_port_obj_vlan_add(struct rocker_port *rocker_port,
				  const struct switchdev_obj_port_vlan *vlan)
{
	return 0;
}

static int secy_port_obj_vlan_del(struct rocker_port *rocker_port,
				  const struct switchdev_obj_port_vlan *vlan)
{
	return 0;
}

static int secy_port_obj_fdb_add(struct rocker_port *rocker_port,
				 u16 vid, const unsigned char *addr)
{
	return 0;
}

static int secy_port_obj_fdb_del(struct rocker_port *rocker_port,
				 u16 vid, const unsigned char *addr)
{
	return 0;
}

static int secy_port_bridge_join(struct secy_port *secy_port,
				 struct net_device *bridge)
{
	return 0;
}

static int secy_port_bridge_leave(struct secy_port *secy_port)
{
	return 0;
}

static int secy_port_ovs_changed(struct secy_port *secy_port,
				 struct net_device *master)
{
	return 0;
}

static int secy_port_master_linked(struct rocker_port *rocker_port,
				   struct net_device *master)
{
	return 0;
}

static int secy_port_master_unlinked(struct rocker_port *rocker_port,
				     struct net_device *master)
{
	return 0;
}

static int secy_port_neigh_update(struct rocker_port *rocker_port,
				  struct neighbour *n)
{
	return 0;
}

static int secy_port_neigh_destroy(struct rocker_port *rocker_port,
				   struct neighbour *n)
{
	return 0;
}

static int secy_port_ev_mac_vlan_seen(struct rocker_port *rocker_port,
				      const unsigned char *addr,
				      __be16 vlan_id)
{
	return 0;
}

static struct secy_port *secy_port_dev_lower_find(struct net_device *dev,
						  struct rocker *rocker)
{
	return NULL;
}

static int secy_fib4_add(struct rocker *rocker,
			 const struct fib_entry_notifier_info *fen_info)
{
	return 0;
}

static int secy_fib4_del(struct rocker *rocker,
			 const struct fib_entry_notifier_info *fen_info)
{
	return 0;
}

static void secy_fib4_abort(struct rocker *rocker)
{
	return;
}

struct rocker_world_ops rocker_secy_ops = {
	.kind = "SecY",
	.priv_size = sizeof(struct secy),
	.port_priv_size = sizeof(struct secy_port),
	.mode = ROCKER_PORT_MODE_SECY,
	.init = secy_init,
	.fini = secy_fini,
	.port_pre_init = secy_port_pre_init,
	.port_init = secy_port_init,
	.port_fini = secy_port_fini,
	.port_open = secy_port_open,
	.port_stop = secy_port_stop,
	.port_attr_stp_state_set = secy_port_attr_stp_state_set,
	.port_attr_bridge_flags_set = secy_port_attr_bridge_flags_set,
	.port_attr_bridge_flags_get = secy_port_attr_bridge_flags_get,
	.port_attr_bridge_flags_support_get = secy_port_attr_bridge_flags_support_get,
	.port_attr_bridge_ageing_time_set = secy_port_attr_bridge_ageing_time_set,
	.port_obj_vlan_add = secy_port_obj_vlan_add,
	.port_obj_vlan_del = secy_port_obj_vlan_del,
	.port_obj_fdb_add = secy_port_obj_fdb_add,
	.port_obj_fdb_del = secy_port_obj_fdb_del,
	.port_master_linked = secy_port_master_linked,
	.port_master_unlinked = secy_port_master_unlinked,
	.port_neigh_update = secy_port_neigh_update,
	.port_neigh_destroy = secy_port_neigh_destroy,
	.port_ev_mac_vlan_seen = secy_port_ev_mac_vlan_seen,
	.fib4_add = secy_fib4_add,
	.fib4_del = secy_fib4_del,
	.fib4_abort = secy_fib4_abort,
};
