/*
 * drivers/net/ethernet/rocker/rocker_secy.c - Rocker switch SecY support
 *
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
#include <linux/if_macsec.h>
#include <net/switchdev.h>

#include "rocker.h"
#include "rocker_tlv.h"

/* No need to hold IEEE 802.1X SecY states as those are fully
 * maintained by the upper MACsec driver except that they don't
 * distinguish Virtual Ports. As far as SCI is identified when
 * each switchdev API is called, rocker device driver can remain
 * stateless, being just a THIN command/event channel. HW device,
 * as represented by QEMU rocker device, demultiplexes to each
 * Virtual Ports, or multiplexes to deliver to rocker_ports here.
 */
struct secy_ieee8021x {
	struct rocker *rocker;
};

/* SecY Multiplexer, facing media access method-specific functions. */
struct secy_mux {
	struct secy_ieee8021x *secy_ieee8021x;
	struct rocker_port *rocker_port;
	struct net_device *dev;
};

#define SECY_OP_FLAG_REMOVE	BIT(0)
#define SECY_OP_FLAG_LEARNED	BIT(1)
#define SECY_OP_FLAG_ON_VPORT	BIT(2)

/**********************************
 * Rocker commands
 **********************************/
struct sc_cmd {
	u64 sci;
	u64 tx_sci;
	bool is_tx;
	bool is_adding;
};

struct sa_cmd {
	u64 sci;
	u8 an;
	u32 pn;
	bool is_adding;
	u16 sak_len;
	char *sak;
};

static int secy_cmd_fdb(const struct rocker_port *rocker_port,
			struct rocker_desc_info *desc_info,
			void *priv)
{
	u32 cmd;
	struct rocker_tlv *cmd_info;

	cmd = ROCKER_TLV_CMD_TYPE_SECY_FDB_ADD;

	if (rocker_tlv_put_u32(desc_info, ROCKER_TLV_CMD_TYPE, cmd))
		return -EMSGSIZE;

	cmd_info = rocker_tlv_nest_start(desc_info, ROCKER_TLV_CMD_INFO);
	if (!cmd_info)
		return -EMSGSIZE;

	if (rocker_tlv_put_u32(desc_info, ROCKER_TLV_SECY_PPORT,
			       rocker_port->pport))
		return -EMSGSIZE;

	rocker_tlv_nest_end(desc_info, cmd_info);

	return 0;
}

static int secy_fdb_add(struct secy_mux *mux, int flags)
{
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_fdb,
			       NULL, NULL, NULL);
}

static int secy_fdb_del(struct secy_mux *mux, int flags)
{
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_fdb,
			       NULL, NULL, NULL);
}

static int secy_fdb_do(struct secy_mux *mux, u64 port_id, int flags)
{
	if (flags & SECY_OP_FLAG_REMOVE)
		return secy_fdb_del(mux, flags);
	else
		return secy_fdb_add(mux, flags);
}

static int secy_cmd_sc(const struct rocker_port *rocker_port,
		       struct rocker_desc_info *desc_info,
		       void *priv)
{
	struct sc_cmd *sc_cmd = priv;

	u32 cmd;
	struct rocker_tlv *cmd_info;

	if (sc_cmd->is_tx) {
		if (sc_cmd->is_adding)
			cmd = ROCKER_TLV_CMD_TYPE_SECY_ADD_TX_SC;
		else
			cmd = ROCKER_TLV_CMD_TYPE_SECY_DEL_TX_SC;
	} else {
		if (sc_cmd->is_adding)
			cmd = ROCKER_TLV_CMD_TYPE_SECY_ADD_RX_SC;
		else
			cmd = ROCKER_TLV_CMD_TYPE_SECY_DEL_RX_SC;
	}
	if (rocker_tlv_put_u32(desc_info, ROCKER_TLV_CMD_TYPE, cmd))
		return -EMSGSIZE;

	cmd_info = rocker_tlv_nest_start(desc_info, ROCKER_TLV_CMD_INFO);
	if (!cmd_info)
		return -EMSGSIZE;

	if (rocker_tlv_put_u32(desc_info, ROCKER_TLV_SECY_PPORT,
			       rocker_port->pport))
		return -EMSGSIZE;
	if (rocker_tlv_put_u64(desc_info, ROCKER_TLV_SECY_SCI, sc_cmd->sci))
		return -EMSGSIZE;

	if (sc_cmd->is_tx &&
	    rocker_tlv_put_u8(desc_info, ROCKER_TLV_SECY_TX, 1))
		return -EMSGSIZE;
	if (!sc_cmd->is_tx &&
	    rocker_tlv_put_u64(desc_info, ROCKER_TLV_SECY_TX_SCI,
						 sc_cmd->tx_sci))
		return -EMSGSIZE;

	rocker_tlv_nest_end(desc_info, cmd_info);

	return 0;
}

static int secy_cmd_sa(const struct rocker_port *rocker_port,
		       struct rocker_desc_info *desc_info,
		       void *priv)
{
	struct sa_cmd *sa_cmd = priv;

	u32 cmd;
	struct rocker_tlv *cmd_info;

	if (sa_cmd->is_adding)
		cmd = ROCKER_TLV_CMD_TYPE_SECY_ADD_TX_SA;
	else
		cmd = ROCKER_TLV_CMD_TYPE_SECY_DEL_TX_SA;

	if (rocker_tlv_put_u32(desc_info, ROCKER_TLV_CMD_TYPE, cmd))
		return -EMSGSIZE;

	cmd_info = rocker_tlv_nest_start(desc_info, ROCKER_TLV_CMD_INFO);
	if (!cmd_info)
		return -EMSGSIZE;

	if (rocker_tlv_put_u64(desc_info, ROCKER_TLV_SECY_SCI, sa_cmd->sci))
		return -EMSGSIZE;

	if (rocker_tlv_put_u64(desc_info, ROCKER_TLV_SECY_AN, sa_cmd->an))
		return -EMSGSIZE;

	if (rocker_tlv_put_u64(desc_info, ROCKER_TLV_SECY_PN, sa_cmd->pn))
		return -EMSGSIZE;

	if (sa_cmd->sak_len * 8 == MACSEC_MAX_KEY_LEN) {
		u128 *sak = (u128 *)sa_cmd->sak;
		if (rocker_tlv_put_u128(desc_info, ROCKER_TLV_SECY_SAK, *sak))
			return -EMSGSIZE;
		if (rocker_tlv_put_u16(desc_info, ROCKER_TLV_SECY_SAK_LEN,
				       sa_cmd->sak_len))
			return -EMSGSIZE;
	} else if (!sa_cmd->sak_len) {
		if (rocker_tlv_put_u128(desc_info, ROCKER_TLV_SECY_SAK,
					(u128){0, 0}))
			return -EMSGSIZE;
		if (rocker_tlv_put_u16(desc_info, ROCKER_TLV_SECY_SAK_LEN, 0))
			return -EMSGSIZE;
	} else { /* currently macsec driver supports GCM-AES-128 only */
		return -ENOTSUPP;
	}

	rocker_tlv_nest_end(desc_info, cmd_info);

	return 0;
}

static int secy_mux_txsc_add(struct secy_mux *mux, u64 sci) {
	struct sc_cmd cmd = {
		.sci = sci,
		.is_tx = true,
		.is_adding = true,
	};
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_sc, &cmd,
			       NULL, NULL);
}

static int secy_mux_txsc_del(struct secy_mux *mux, u64 sci) {
	struct sc_cmd cmd = {
		.sci = sci,
		.is_tx = true,
		.is_adding = false
	};
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_sc, &cmd,
			       NULL, NULL);
}

static int secy_mux_rxsc_add(struct secy_mux *mux, u64 sci, u64 tx_sci) {
	struct sc_cmd cmd = {
		.sci = sci,
		.tx_sci = tx_sci,
		.is_tx = false,
		.is_adding = true,
	};
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_sc, &cmd,
			       NULL, NULL);
}

static int secy_mux_rxsc_del(struct secy_mux *mux, u64 sci) {
	struct sc_cmd cmd = {
		.sci = sci,
		.is_tx = false,
		.is_adding = false,
	};
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_sc, &cmd,
			       NULL, NULL);
}

static int secy_mux_sa_add(struct secy_mux *mux, u64 sci, u8 an, u32 pn,
			   u16 sak_len, char *sak) {
	struct sa_cmd cmd = {
		.sci = sci,
		.an = an,
		.pn = pn,
		.sak_len = sak_len,
		.sak = sak,
		.is_adding = true,
	};
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_sa, &cmd,
			       NULL, NULL);
}

static int secy_mux_sa_del(struct secy_mux *mux, u64 sci, u8 an, u32 pn) {
	struct sa_cmd cmd = {
		.sci = sci,
		.an = an,
		.pn = pn,
		.is_adding = false,
	};
	return rocker_cmd_exec(mux->rocker_port, false, secy_cmd_sa, &cmd,
			       NULL, NULL);
}

/**********************************
 * FDB, VLAN and neigh
 **********************************/
struct secy_fdb_learn_work {
	struct work_struct work;
	struct net_device *dev;
	int flags;
	u8 addr[ETH_ALEN];
	u16 vid;
	u64 sci;
};

static void secy_fdb_learn_work(struct work_struct *work)
{
	const struct secy_fdb_learn_work *lw =
		container_of(work, struct secy_fdb_learn_work, work);
	bool removing = (lw->flags & SECY_OP_FLAG_REMOVE);
	bool learned = (lw->flags & SECY_OP_FLAG_LEARNED);
	bool on_vport = (lw->flags & SECY_OP_FLAG_ON_VPORT);
	struct switchdev_notifier_fdb_info info;

	info.addr = lw->addr;
	info.vid = lw->vid;
	if (on_vport) {
		info.info.port_id_active = true;
		info.info.port_id = lw->sci;
	}

	rtnl_lock();
	if (learned && removing)
		call_switchdev_notifiers(SWITCHDEV_FDB_DEL_TO_BRIDGE,
					 lw->dev, &info.info);
	else
		call_switchdev_notifiers(SWITCHDEV_FDB_ADD_TO_BRIDGE,
					 lw->dev, &info.info);
	rtnl_unlock();

	kfree(work);
}

static int secy_fdb_learn(struct secy_mux *mux, int flags, const u8 *addr,
			  __be16 vlan_id, u64 port_id)
{
	struct secy_fdb_learn_work *lw;
	int err;

	err = secy_fdb_do(mux, port_id, flags);
	if (err)
		return err;

	lw = kzalloc(sizeof(*lw), GFP_ATOMIC);
	if (!lw)
		return -ENOMEM;

	INIT_WORK(&lw->work, secy_fdb_learn_work);

	lw->dev = mux->dev;
	lw->flags = flags;
	ether_addr_copy(lw->addr, addr);
	lw->vid = vlan_id;
	lw->sci = port_id;

	schedule_work(&lw->work);
	return 0;
}

static int secy_fdb(struct secy_mux *mux, const unsigned char *addr,
		    __be16 vlan_id, u64 port_id, int flags)
{
	/* */
	return secy_fdb_learn(mux, flags, addr, vlan_id, port_id);
}

/**********************************
 * Rocker world ops implementation
 **********************************/

static int secy_ieee8021x_init(struct rocker *rocker)
{
	struct secy_ieee8021x *secy_ieee8021x = rocker->wpriv;
	secy_ieee8021x->rocker = rocker;
	return 0;
}

static void secy_ieee8021x_fini(struct rocker *rocker)
{
}

static int secy_mux_pre_init(struct rocker_port *rocker_port)
{
	struct secy_mux *secy_mux = rocker_port->wpriv;

	secy_mux->secy_ieee8021x = rocker_port->rocker->wpriv;
	secy_mux->rocker_port = rocker_port;
	secy_mux->dev = rocker_port->dev;
	return 0;
}

static int secy_mux_init(struct rocker_port *rocker_port)
{
	return 0;
}

static void secy_mux_fini(struct rocker_port *rocker_port)
{
}

static int secy_mux_open(struct rocker_port *rocker_port)
{
	/* No Uncontrolled Port must have not been opened until some SecY
	 * instance is born. */
	return 0;
}

static void secy_mux_stop(struct rocker_port *rocker_port)
{
}

static int secy_mux_obj_fdb_add(struct rocker_port *rocker_port, u16 vid,
			    const unsigned char *addr)
{
	struct secy_mux *mux = rocker_port->wpriv;
	__be16 vlan_id = htons(vid);

	return secy_fdb(mux, addr, vlan_id, 0, 0);
}

static int secy_mux_obj_fdb_del(struct rocker_port *rocker_port, u16 vid,
			    const unsigned char *addr)
{
	struct secy_mux *mux = rocker_port->wpriv;
	__be16 vlan_id = htons(vid);
	int flags = SECY_OP_FLAG_REMOVE;

	return secy_fdb(mux, addr, vlan_id, 0, flags);
}

static int secy_mux_obj_vport_fdb_add(
		struct rocker_port *rocker_port, u64 port_id,
		u16 vid, const unsigned char *addr)
{
	struct secy_mux *mux = rocker_port->wpriv;
	__be16 vlan_id = htons(vid);
	int flags = SECY_OP_FLAG_ON_VPORT;

	return secy_fdb(mux, addr, vlan_id, port_id, flags);
}

static int secy_mux_obj_vport_fdb_del(
		struct rocker_port *rocker_port, u64 port_id,
		u16 vid, const unsigned char *addr)
{
	struct secy_mux *mux = rocker_port->wpriv;
	__be16 vlan_id = htons(vid);
	int flags = SECY_OP_FLAG_ON_VPORT | SECY_OP_FLAG_REMOVE;

	return secy_fdb(mux, addr, vlan_id, port_id, flags);
}

static int secy_mux_obj_txsc_add(
		struct rocker_port *rocker_port,
		const struct switchdev_obj_port_secy_txsc *txsc)
{
	int err;
	struct secy_mux *mux = rocker_port->wpriv;

	err = secy_mux_txsc_add(mux, txsc->sci);
	if (err)
		return err;

	return 0;
}

static int secy_mux_obj_txsc_del(
		struct rocker_port *rocker_port,
		const struct switchdev_obj_port_secy_txsc *txsc)
{
	int err;
	struct secy_mux *mux = rocker_port->wpriv;

	err = secy_mux_txsc_del(mux, txsc->sci);
	if (err)
		return err;

	return 0;
}

static int secy_mux_obj_rxsc_add(
		struct rocker_port *rocker_port,
		const struct switchdev_obj_port_secy_rxsc *rxsc)
{
	int err;
	struct secy_mux *mux = rocker_port->wpriv;

	err = secy_mux_rxsc_add(mux, rxsc->sci, rxsc->tx_sci);
	if (err)
		return err;

	return 0;
}

static int secy_mux_obj_rxsc_del(
		struct rocker_port *rocker_port,
		const struct switchdev_obj_port_secy_rxsc *rxsc)
{
	int err;
	struct secy_mux *mux = rocker_port->wpriv;

	err = secy_mux_rxsc_del(mux, rxsc->sci);
	if (err)
		return err;

	return 0;
}

static int secy_mux_obj_sa_add(
		struct rocker_port *rocker_port,
		const struct switchdev_obj_port_secy_sa *sa)
{
	int err;
	struct secy_mux *mux = rocker_port->wpriv;

	err = secy_mux_sa_add(mux, sa->sci, sa->an, sa->pn,
			      sa->sak_len, sa->sak);
	if (err)
		return err;

	return 0;
}

static int secy_mux_obj_sa_del(
		struct rocker_port *rocker_port,
		const struct switchdev_obj_port_secy_sa *sa)
{
	int err;
	struct secy_mux *mux = rocker_port->wpriv;

	err = secy_mux_sa_del(mux, sa->sci, sa->an, sa->pn);
	if (err)
		return err;

	return 0;
}

struct rocker_world_ops rocker_secy_ops = {
	.kind = "SecY",
	.priv_size = sizeof(struct secy_ieee8021x),
	.port_priv_size = sizeof(struct secy_mux),
	.mode = ROCKER_PORT_MODE_SECY,
	.init = secy_ieee8021x_init,
	.fini = secy_ieee8021x_fini,
	.port_pre_init = secy_mux_pre_init,
	.port_init = secy_mux_init,
	.port_fini = secy_mux_fini,
	.port_open = secy_mux_open,
	.port_stop = secy_mux_stop,
	.port_obj_fdb_add = secy_mux_obj_fdb_add,
	.port_obj_fdb_del = secy_mux_obj_fdb_del,
	.port_obj_vport_fdb_add = secy_mux_obj_vport_fdb_add,
	.port_obj_vport_fdb_del = secy_mux_obj_vport_fdb_del,
	.port_obj_secy_txsc_add = secy_mux_obj_txsc_add,
	.port_obj_secy_txsc_del = secy_mux_obj_txsc_del,
	.port_obj_secy_rxsc_add = secy_mux_obj_rxsc_add,
	.port_obj_secy_rxsc_del = secy_mux_obj_rxsc_del,
	.port_obj_secy_sa_add = secy_mux_obj_sa_add,
	.port_obj_secy_sa_del = secy_mux_obj_sa_del,
};
