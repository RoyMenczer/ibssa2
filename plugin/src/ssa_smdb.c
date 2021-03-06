/*
 * Copyright (c) 2011-2015 Mellanox Technologies LTD. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <infiniband/ssa_smdb_api.h>
#include <asm/byteorder.h>

extern struct db_table_def	ip_def_tbl[];
extern struct db_dataset	ip_dataset_tbl[];
extern struct db_dataset	ip_field_dataset_tbl[];
extern struct db_field_def	ip_field_tbl[];

static struct db_table_def def_tbl[] = {
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_SUBNET_OPTS, 0 },
		"SUBNET_OPTS", __constant_htonl(sizeof(struct smdb_subnet_opts)), 0 },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DEF, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, 0 },
		"SUBNET_OPTS_fields", __constant_htonl(sizeof(struct db_field_def)), __constant_htonl(SMDB_TBL_ID_SUBNET_OPTS) },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_GUID2LID, 0 },
		"GUID_to_LID", __constant_htonl(sizeof(struct smdb_guid2lid)), 0 },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DEF, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_GUID2LID, 0 },
		"GUID_to_LID_fields", __constant_htonl(sizeof(struct db_field_def)), __constant_htonl(SMDB_TBL_ID_GUID2LID) },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_NODE, 0 },
		"NODE", __constant_htonl(sizeof(struct smdb_node)), 0 },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DEF, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_NODE, 0 },
		"NODE_fields", __constant_htonl(sizeof(struct db_field_def)), __constant_htonl(SMDB_TBL_ID_NODE) },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_LINK, 0 },
		"LINK", __constant_htonl(sizeof(struct smdb_link)), 0 },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DEF, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LINK, 0 },
		"LINK_fields", __constant_htonl(sizeof(struct db_field_def)), __constant_htonl(SMDB_TBL_ID_LINK) },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_PORT, 0 },
		"PORT", __constant_htonl(sizeof(struct smdb_port)), 0 },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DEF, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, 0 },
		"PORT_fields", __constant_htonl(sizeof(struct db_field_def)), __constant_htonl(SMDB_TBL_ID_PORT) },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_PKEY, 0 },
		"PKEY", __constant_htonl(DB_VARIABLE_SIZE), __constant_htonl(SMDB_TBL_ID_PORT) },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_LFT_TOP, 0 },
		"LFT_TOP", __constant_htonl(sizeof(struct smdb_lft_top)), 0 },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DEF, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_TOP, 0 },
		"LFT_TOP_fields", __constant_htonl(sizeof(struct db_field_def)), __constant_htonl(SMDB_TBL_ID_LFT_TOP) },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DATA, 0, { 0, SMDB_TBL_ID_LFT_BLOCK, 0 },
		"LFT_BLOCK", __constant_htonl(sizeof(struct smdb_lft_block)), 0 },
	{ DBT_DEF_VERSION, sizeof(struct db_table_def), DBT_TYPE_DEF, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_BLOCK, 0 },
		"LFT_BLOCK_fields", __constant_htonl(sizeof(struct db_field_def)), __constant_htonl(SMDB_TBL_ID_LFT_BLOCK) },
	[SMDB_TBLS] = { DB_VERSION_INVALID }
};

static struct db_dataset dataset_tbl[] = {
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_SUBNET_OPTS, 0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_GUID2LID,    0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_NODE,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_LINK,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_PORT,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_PKEY,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_LFT_TOP,     0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_LFT_BLOCK,   0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	[SMDB_DATA_TBLS] = { DB_VERSION_INVALID }
};

static struct db_dataset field_dataset_tbl[] = {
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, 0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_GUID2LID,    0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_NODE,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LINK,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PKEY,        0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_TOP,     0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	{ DB_DS_VERSION, sizeof(struct db_dataset), 0, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_BLOCK,   0 }, DB_EPOCH_INVALID, 0, 0, 0 },
	[SMDB_DATA_TBLS] = { DB_VERSION_INVALID }
};

static struct db_field_def field_tbl[] = {
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET64, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, SMDB_FIELD_ID_SUBNET_OPTS_CHANGE_MASK }, "change_mask", __constant_htonl(64), 0 },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET64, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, SMDB_FIELD_ID_SUBNET_OPTS_SUBNET_PREFIX }, "subnet_prefix", __constant_htonl(64), __constant_htonl(64) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, SMDB_FIELD_ID_SUBNET_OPTS_SM_STATE }, "sm_state", __constant_htonl(8), __constant_htonl(128) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, SMDB_FIELD_ID_SUBNET_OPTS_LMC }, "lmc", __constant_htonl(8), __constant_htonl(136) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, SMDB_FIELD_ID_SUBNET_OPTS_SUBNET_TIMEOUT }, "subnet_timeout", __constant_htonl(8), __constant_htonl(144) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_SUBNET_OPTS, SMDB_FIELD_ID_SUBNET_OPTS_ALLOW_BOTH_PKEYS }, "allow_both_pkeys", __constant_htonl(8), __constant_htonl(152) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET64, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_GUID2LID, SMDB_FIELD_ID_GUID2LID_GUID }, "guid", __constant_htonl(64), 0 },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_GUID2LID, SMDB_FIELD_ID_GUID2LID_LID }, "lid", __constant_htonl(16), __constant_htonl(64) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_GUID2LID, SMDB_FIELD_ID_GUID2LID_LMC }, "lmc", __constant_htonl(8), __constant_htonl(80) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_GUID2LID, SMDB_FIELD_ID_GUID2LID_IS_SWITCH }, "is_switch", __constant_htonl(8), __constant_htonl(88) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET64, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_NODE, SMDB_FIELD_ID_NODE_NODE_GUID }, "node_guid", __constant_htonl(64), 0 },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_NODE, SMDB_FIELD_ID_NODE_IS_ENHANCED_SP0 }, "is_enhanced_sp0", __constant_htonl(8), __constant_htonl(64) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_NODE, SMDB_FIELD_ID_NODE_NODE_TYPE }, "node_type", __constant_htonl(8), __constant_htonl(72) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_NODE, SMDB_FIELD_ID_NODE_IS_ENHANCED_SP0 }, "description", __constant_htonl(8 * IB_NODE_DESCRIPTION_SIZE), __constant_htonl(80) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LINK, SMDB_FIELD_ID_LINK_FROM_LID }, "from_lid", __constant_htonl(16), 0 },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LINK, SMDB_FIELD_ID_LINK_TO_LID }, "to_lid", __constant_htonl(16), __constant_htonl(16) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LINK, SMDB_FIELD_ID_LINK_FROM_PORT_NUM }, "from_port_num", __constant_htonl(8), __constant_htonl(32) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LINK, SMDB_FIELD_ID_LINK_TO_PORT_NUM }, "to_port_num", __constant_htonl(8), __constant_htonl(40) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET64, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, SMDB_FIELD_ID_PORT_PKEY_TBL_OFFSET }, "pkey_tbl_offset", __constant_htonl(64), 0 },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, SMDB_FIELD_ID_PORT_PKEY_TBL_SIZE }, "pkey_tbl_size", __constant_htonl(16), __constant_htonl(64) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, SMDB_FIELD_ID_PORT_PORT_LID }, "port_lid", __constant_htonl(16), __constant_htonl(80) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, SMDB_FIELD_ID_PORT_PORT_NUM }, "port_num", __constant_htonl(8), __constant_htonl(96) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, SMDB_FIELD_ID_PORT_MTU_CAP }, "mtu_cap", __constant_htonl(8), __constant_htonl(104) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, SMDB_FIELD_ID_PORT_RATE }, "rate", __constant_htonl(8), __constant_htonl(112) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_PORT, SMDB_FIELD_ID_PORT_VL_ENFORCE }, "vl_enforce", __constant_htonl(8), __constant_htonl(120) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_TOP, SMDB_FIELD_ID_LFT_TOP_LID }, "lid", __constant_htonl(16), 0 },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_TOP, SMDB_FIELD_ID_LFT_TOP_LFT_TOP }, "lft_top", __constant_htonl(16), __constant_htonl(16) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_BLOCK, SMDB_FIELD_ID_LFT_BLOCK_LID }, "lid", __constant_htonl(16), 0 },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_NET16, 0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_BLOCK, SMDB_FIELD_ID_LFT_BLOCK_BLOCK_NUM }, "block_num", __constant_htonl(16), __constant_htonl(16) },
	{ DBF_DEF_VERSION, 0, DBF_TYPE_U8,    0, { 0, SMDB_TBL_ID_MAX + SMDB_TBL_ID_LFT_BLOCK, SMDB_FIELD_ID_LFT_BLOCK_BLOCK }, "block", __constant_htonl(8 * UMAD_LEN_SMP_DATA), __constant_htonl(32) },
	[SMDB_FIELDS] = { DB_VERSION_INVALID }
};

static void smdb_attach_ipdb()
{
	int i = 0;
	uint8_t offset;

	offset = SMDB_TBL_OFFSET * 2 - 1; /* no field table for pkey record */
	for (i = offset; i < SMDB_TBLS; i++) {
		def_tbl[i] = ip_def_tbl[i - offset];
		if (def_tbl[i].type == DBT_TYPE_DATA) {
			def_tbl[i].id.table += SMDB_TBL_OFFSET;
		} if (def_tbl[i].type == DBT_TYPE_DEF) {
			def_tbl[i].id.table -= IPDB_TBL_ID_MAX;
			def_tbl[i].id.table += SMDB_DATA_TBLS + SMDB_TBL_OFFSET;
			def_tbl[i].ref_table_id =
				htonl(ntohl(def_tbl[i].ref_table_id) +
					    SMDB_TBL_OFFSET);
		}
	}

	offset = SMDB_TBL_OFFSET;
	for (i = offset; i < SMDB_DATA_TBLS; i++) {
		dataset_tbl[i] = ip_dataset_tbl[i - offset];
		dataset_tbl[i].id.table += offset;
	}

	offset = SMDB_TBL_OFFSET;
	for (i = offset; i < SMDB_DATA_TBLS; i++) {
		field_dataset_tbl[i] = ip_field_dataset_tbl[i - offset];
		field_dataset_tbl[i].id.table -= IPDB_TBL_ID_MAX;
		field_dataset_tbl[i].id.table +=
			SMDB_DATA_TBLS + SMDB_TBL_OFFSET;
	}

	offset = SMDB_FIELDS - IPDB_FIELDS;
	for (i = offset; i < SMDB_FIELDS; i++) {
		field_tbl[i] = ip_field_tbl[i - offset];
		field_tbl[i].id.table -= IPDB_TBL_ID_MAX;
		field_tbl[i].id.table += SMDB_DATA_TBLS +
					 SMDB_TBL_OFFSET;
	}
}

/** =========================================================================
 */
struct ssa_db *ssa_db_smdb_init(uint64_t epoch, uint64_t data_rec_cnt[SMDB_DATA_TBLS])
{
	struct ssa_db *p_ssa_db;
	uint64_t num_field_recs_arr[SMDB_DATA_TBLS];
	size_t recs_size_arr[SMDB_DATA_TBLS];

	recs_size_arr[SMDB_TBL_ID_SUBNET_OPTS]	= sizeof(struct smdb_subnet_opts);
	recs_size_arr[SMDB_TBL_ID_GUID2LID]	= sizeof(struct smdb_guid2lid);
	recs_size_arr[SMDB_TBL_ID_NODE]		= sizeof(struct smdb_node);
	recs_size_arr[SMDB_TBL_ID_LINK]		= sizeof(struct smdb_link);
	recs_size_arr[SMDB_TBL_ID_PORT]		= sizeof(struct smdb_port);
	recs_size_arr[SMDB_TBL_ID_PKEY]		= sizeof(uint16_t);
	recs_size_arr[SMDB_TBL_ID_LFT_TOP]	= sizeof(struct smdb_lft_top);
	recs_size_arr[SMDB_TBL_ID_LFT_BLOCK]	= sizeof(struct smdb_lft_block);
	recs_size_arr[SMDB_TBL_ID_IPv4]		= sizeof(struct ipdb_ipv4);
	recs_size_arr[SMDB_TBL_ID_IPv6]		= sizeof(struct ipdb_ipv6);
	recs_size_arr[SMDB_TBL_ID_NAME]		= sizeof(struct ipdb_name);

	num_field_recs_arr[SMDB_TBL_ID_SUBNET_OPTS]	= SMDB_FIELD_ID_SUBNET_OPTS_MAX;
	num_field_recs_arr[SMDB_TBL_ID_GUID2LID]	= SMDB_FIELD_ID_GUID2LID_MAX;
	num_field_recs_arr[SMDB_TBL_ID_NODE]		= SMDB_FIELD_ID_NODE_MAX;
	num_field_recs_arr[SMDB_TBL_ID_LINK]		= SMDB_FIELD_ID_LINK_MAX;
	num_field_recs_arr[SMDB_TBL_ID_PORT]		= SMDB_FIELD_ID_PORT_MAX;
	num_field_recs_arr[SMDB_TBL_ID_PKEY]		= DB_VARIABLE_SIZE; /* variable size records */
	num_field_recs_arr[SMDB_TBL_ID_LFT_TOP]		= SMDB_FIELD_ID_LFT_TOP_MAX;
	num_field_recs_arr[SMDB_TBL_ID_LFT_BLOCK]	= SMDB_FIELD_ID_LFT_BLOCK_MAX;
	num_field_recs_arr[SMDB_TBL_ID_IPv4]		= IPDB_FIELD_ID_IPv4_MAX;
	num_field_recs_arr[SMDB_TBL_ID_IPv6]		= IPDB_FIELD_ID_IPv6_MAX;
	num_field_recs_arr[SMDB_TBL_ID_NAME]		= IPDB_FIELD_ID_NAME_MAX;

	p_ssa_db = ssa_db_alloc(data_rec_cnt, recs_size_arr,
				num_field_recs_arr, SMDB_TBL_ID_MAX);

	smdb_attach_ipdb();

	ssa_db_init(p_ssa_db, "SMDB", 12 /* just some db_id */, epoch, def_tbl,
		    dataset_tbl, field_dataset_tbl, field_tbl);

	return p_ssa_db;
}

/** =========================================================================
 */
void ssa_db_smdb_destroy(struct ssa_db * p_smdb)
{
	ssa_db_destroy(p_smdb);
}

/** =========================================================================
 */
void smdb_subnet_opts_init(osm_subn_t * p_subn, struct smdb_subnet_opts * p_rec)
{
	p_rec->change_mask = 0;
	p_rec->subnet_prefix = p_subn->opt.subnet_prefix;
	p_rec->sm_state = p_subn->sm_state;
	p_rec->lmc = p_subn->opt.lmc;
	p_rec->subnet_timeout = p_subn->opt.subnet_timeout;
	p_rec->allow_both_pkeys = (uint8_t) p_subn->opt.allow_both_pkeys;

	memset(&p_rec->pad, 0, sizeof(p_rec->pad));
}

/** =========================================================================
 */
void smdb_guid2lid_init(osm_port_t *p_port, struct smdb_guid2lid *p_rec)
{
	p_rec->guid = osm_physp_get_port_guid(p_port->p_physp);
	p_rec->lid = osm_physp_get_base_lid(p_port->p_physp);
	p_rec->lmc = osm_physp_get_lmc(p_port->p_physp);
	p_rec->is_switch = (osm_node_get_type(p_port->p_node) == IB_NODE_TYPE_SWITCH);

	memset(&p_rec->pad, 0, sizeof(p_rec->pad));
}

/** =========================================================================
 */
void smdb_node_init(osm_node_t *p_node, struct smdb_node *p_rec)
{
	p_rec->node_guid = osm_node_get_node_guid(p_node);
	if (p_node->node_info.node_type == IB_NODE_TYPE_SWITCH)
		p_rec->is_enhanced_sp0 =
			ib_switch_info_is_enhanced_port0(&p_node->sw->switch_info);
	else
		p_rec->is_enhanced_sp0 = 0;
	p_rec->node_type = p_node->node_info.node_type;
	memcpy(p_rec->description, p_node->node_desc.description,
	       sizeof(p_rec->description));
	memset(&p_rec->pad, 0, sizeof(p_rec->pad));
}

/** =========================================================================
 */
void smdb_link_init(osm_physp_t *p_physp, struct smdb_link *p_rec)
{
	osm_physp_t *p_remote_physp;

	if (osm_node_get_type(p_physp->p_node) == IB_NODE_TYPE_SWITCH) {
		p_rec->from_lid = osm_node_get_base_lid(p_physp->p_node, 0);
		p_rec->from_port_num = osm_physp_get_port_num(p_physp);
	} else {
		p_rec->from_lid = osm_physp_get_base_lid(p_physp);
		p_rec->from_port_num = 0;
	}

	p_remote_physp = osm_physp_get_remote(p_physp);

	if (osm_node_get_type(p_remote_physp->p_node) == IB_NODE_TYPE_SWITCH) {
		p_rec->to_lid = osm_node_get_base_lid(p_remote_physp->p_node, 0);
		p_rec->to_port_num = osm_physp_get_port_num(p_remote_physp);
	} else {
		p_rec->to_lid = osm_physp_get_base_lid(p_remote_physp);
		p_rec->to_port_num = 0;
	}
	memset(&p_rec->pad, 0, sizeof(p_rec->pad));
}

/** =========================================================================
 */
void smdb_port_init(osm_physp_t *p_physp, uint64_t pkey_base_offset,
		    uint16_t pkey_tbl_size, uint16_t lid,
		    struct smdb_port *p_rec)
{
	const ib_port_info_t *p_pi;
	const osm_physp_t *p_physp0;
	uint8_t is_fdr10_active;
	uint8_t is_switch;

	if (osm_node_get_type(p_physp->p_node) == IB_NODE_TYPE_SWITCH &&
	    osm_physp_get_port_num(p_physp) > 0) {
		/* for SW external ports, port 0 Capability Mask is used  */
		p_physp0 = osm_node_get_physp_ptr((osm_node_t *)p_physp->p_node, 0);
		p_pi = &p_physp0->port_info;
	} else {
		p_pi = &p_physp->port_info;
	}

	is_fdr10_active = ((p_physp->ext_port_info.link_speed_active & FDR10) ? 0xff : 0) &
					  SSA_DB_PORT_IS_FDR10_ACTIVE_MASK;
	is_switch = ((osm_node_get_type(p_physp->p_node) == IB_NODE_TYPE_SWITCH) ? 0xff : 0) &
					  SSA_DB_PORT_IS_SWITCH_MASK;

	p_rec->pkey_tbl_offset		= pkey_base_offset;
	p_rec->pkey_tbl_size		= pkey_tbl_size;
	p_rec->port_lid			=
	    (lid ? lid : osm_physp_get_base_lid(p_physp));
	p_rec->port_num			= osm_physp_get_port_num(p_physp);
	p_rec->mtu_cap			= ib_port_info_get_mtu_cap(&p_physp->port_info);
	p_rec->rate			= ib_port_info_compute_rate(&p_physp->port_info,
								    p_pi->capability_mask & IB_PORT_CAP_HAS_EXT_SPEEDS) &
					  SSA_DB_PORT_RATE_MASK;
	p_rec->vl_enforce		= p_physp->port_info.vl_enforce;
	p_rec->rate			= (uint8_t) (p_rec->rate | is_fdr10_active | is_switch);
}

/** =========================================================================
 */
void smdb_lft_block_init(osm_switch_t * p_sw, uint16_t lid, uint16_t block,
			 struct smdb_lft_block *p_rec)
{
	p_rec->lid		= htons(lid);
	p_rec->block_num	= htons(block);
	memcpy(p_rec->block, p_sw->lft + block * UMAD_LEN_SMP_DATA, UMAD_LEN_SMP_DATA);
}

/** =========================================================================
 */
void smdb_lft_top_init(uint16_t lid, uint16_t lft_top, struct smdb_lft_top *p_rec)
{
	p_rec->lid = htons(lid);
	p_rec->lft_top = htons(lft_top);
}
