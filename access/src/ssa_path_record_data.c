/*
 * Copyright 2004-2015 Mellanox Technologies LTD. All rights reserved.
 *
 * This software is available to you under the terms of the
 * OpenIB.org BSD license included below:
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

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <infiniband/ssa_smdb.h>
#include "ssa_path_record_helper.h"
#include "ssa_path_record_data.h"

static size_t find_port_index(const struct ssa_pr_smdb_index *p_index,
			      const be16_t lid, const unsigned int port_num);

inline static size_t get_dataset_count(const struct ssa_db *p_smdb,
				       unsigned int table_id)
{
	SSA_ASSERT(p_smdb);
	SSA_ASSERT(table_id < SMDB_TBL_ID_MAX);
	SSA_ASSERT(&p_smdb->p_db_tables[table_id]);

	return ntohll(p_smdb->p_db_tables[table_id].set_count);
}

static int build_is_switch_lookup(struct ssa_pr_smdb_index *p_index,
				  const struct ssa_db *p_smdb)
{
	size_t i = 0, count = 0;
	const struct smdb_guid2lid *p_guid2lid_tbl = NULL;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);

	p_guid2lid_tbl =
		(struct smdb_guid2lid *)p_smdb->pp_tables[SMDB_TBL_ID_GUID2LID];
	SSA_ASSERT(p_guid_to_lid_tbl);

	memset(p_index->is_switch_lookup, '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->is_switch_lookup[0]));

	count = get_dataset_count(p_smdb,SMDB_TBL_ID_GUID2LID);
	if (!count) {
		SSA_PR_LOG_ERROR("Guid to LID table is empty");
		return 1;
	}

	for (i = 0; i < count; i++) {
		uint16_t lid = ntohs(p_guid2lid_tbl[i].lid);
		p_index->is_switch_lookup[lid] = p_guid2lid_tbl[i].is_switch;
	}

	return 0;
}

static int build_lft_top_lookup(struct ssa_pr_smdb_index *p_index,
				const struct ssa_db *p_smdb)
{
	size_t i = 0, count = 0;
	struct smdb_lft_top *p_lft_top_tbl = NULL;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);

	p_lft_top_tbl =
		(struct smdb_lft_top *)p_smdb->pp_tables[SMDB_TBL_ID_LFT_TOP];
	SSA_ASSERT(p_lft_top_tbl);

	memset(p_index->lft_top_lookup, '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->lft_top_lookup[0]));

	count = get_dataset_count(p_smdb, SMDB_TBL_ID_LFT_TOP);
	if (!count) {
		SSA_PR_LOG_ERROR("LFT top table is empty");
		return 1;
	}

	for (i = 0; i < count; i++)
		p_index->lft_top_lookup[ntohs(p_lft_top_tbl[i].lid)] = ntohs(p_lft_top_tbl[i].lft_top);

	return 0;
}

static int build_port_index(struct ssa_pr_smdb_index *p_index,
			    const struct ssa_db *p_smdb)
{
	size_t i = 0, count = 0, switch_count = 0;
	const struct smdb_port *p_port_tbl = NULL;
	uint64_t default_val = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);

	p_port_tbl = (struct smdb_port *)p_smdb->pp_tables[SMDB_TBL_ID_PORT];
	SSA_ASSERT(p_port_tbl);

	memset(p_index->ca_port_lookup, '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->ca_port_lookup[0]));
	memset(p_index->switch_port_lookup, '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->switch_port_lookup[0]));

	count = get_dataset_count(p_smdb, SMDB_TBL_ID_PORT);
	if (!count) {
		SSA_PR_LOG_ERROR("Port table is empty");
		return 1;
	}
	default_val = count + 1;

	for (i = 0; i < count; i++) {
		if (p_port_tbl[i].rate & SSA_DB_PORT_IS_SWITCH_MASK) {
			uint64_t *port_lookup = p_index->switch_port_lookup[ntohs(p_port_tbl[i].port_lid)];
			if (!port_lookup) {
				size_t j = 0;

				port_lookup = (uint64_t*)malloc((MAX_LOOKUP_PORT + 1) * sizeof(uint64_t));
				for (j = 0; j <= MAX_LOOKUP_PORT; ++j)
					port_lookup[j] = default_val;

				p_index->switch_port_lookup[ntohs(p_port_tbl[i].port_lid)] = port_lookup;
				switch_count++;
			}
			port_lookup[p_port_tbl[i].port_num] = i;
		} else {
			p_index->ca_port_lookup[ntohs(p_port_tbl[i].port_lid)] = i;
		}
	}

	SSA_PR_LOG_INFO("Switch ports lookup table size: %"PRIu64" bytes",
			switch_count * sizeof(uint64_t) * MAX_LOOKUP_PORT);

	return 0;
}

static int build_lft_block_lookup(struct ssa_pr_smdb_index *p_index,
				  const struct ssa_db *p_smdb)
{
	size_t i = 0, count = 0;
	const struct smdb_lft_block *p_lft_block_tbl = NULL;
	size_t lookup_size = 0;
	uint64_t default_val = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);

	p_lft_block_tbl =(struct smdb_lft_block *)p_smdb->pp_tables[SMDB_TBL_ID_LFT_BLOCK];
	SSA_ASSERT(p_lft_block_tbl);

	memset(p_index->lft_block_lookup, '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->lft_block_lookup[0]));

	count = get_dataset_count(p_smdb, SMDB_TBL_ID_LFT_BLOCK);
	if (!count) {
		SSA_PR_LOG_ERROR("LFT block table is empty");
		return 1;
	}
	default_val = count + 1;

	for (i = 0; i < count; i++) {
		size_t j = 0;
		uint64_t *block_lookup =
			p_index->lft_block_lookup[ntohs(p_lft_block_tbl[i].lid)];

		if (!block_lookup) {
			block_lookup = (uint64_t *)malloc(MAX_LFT_BLOCK_NUM * sizeof(uint64_t));
			lookup_size += MAX_LFT_BLOCK_NUM * sizeof(uint64_t);

			p_index->lft_block_lookup[ntohs(p_lft_block_tbl[i].lid)] = block_lookup;

			for (j = 0; j < MAX_LFT_BLOCK_NUM; ++j)
				block_lookup[j] = default_val;
		}
		block_lookup[ntohs(p_lft_block_tbl[i].block_num)] = i;
	}

	SSA_PR_LOG_INFO("LFT lookup size: %"PRIu64" bytes", lookup_size);
	return 0;
}
static int build_link_index(struct ssa_pr_smdb_index *p_index,
			    const struct ssa_db *p_smdb)
{
	size_t i = 0, link_count = 0, port_count = 0;
	const struct smdb_link *p_link_tbl = NULL;
	uint64_t default_val = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);
	SSA_ASSERT(p_index->is_switch_lookup);

	memset(p_index->ca_link_lookup, '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->ca_link_lookup[0]));
	memset(p_index->switch_link_lookup, '\0',
	       (MAX_LOOKUP_PORT + 1) * sizeof(p_index->switch_link_lookup[0]));

	p_link_tbl = (const struct smdb_link *)p_smdb->pp_tables[SMDB_TBL_ID_LINK];
	SSA_ASSERT(p_link_tbl);

	link_count = get_dataset_count(p_smdb, SMDB_TBL_ID_LINK);
	if (!link_count) {
		SSA_PR_LOG_ERROR("Link table is empty");
		return 1;
	}
	port_count = get_dataset_count(p_smdb, SMDB_TBL_ID_PORT);
	if (!port_count) {
		SSA_PR_LOG_ERROR("Port table is empty");
		return 1;
	}
	default_val = port_count + 1;

	for (i = 0; i < link_count; i++) {
		size_t to_port_index = find_port_index(p_index,
						       p_link_tbl[i].to_lid,
						       p_link_tbl[i].to_port_num);

		if (to_port_index >= port_count) {
			SSA_PR_LOG_ERROR("Can't find port for LID: %u. Link index build failed",
					 ntohs(p_link_tbl[i].to_lid));
			return -1;
		}

		if (p_index->is_switch_lookup[ntohs(p_link_tbl[i].from_lid)]) {
			uint64_t *port_lookup = p_index->switch_link_lookup[ntohs(p_link_tbl[i].from_lid)];
			if (!port_lookup) {
				size_t j = 0;
				port_lookup = (uint64_t *)malloc((MAX_LOOKUP_PORT + 1) * sizeof(uint64_t));
				p_index->switch_link_lookup[ntohs(p_link_tbl[i].from_lid)] = port_lookup;
				for (j = 0; j <= MAX_LOOKUP_PORT; ++j)
					port_lookup[j] = default_val;
			}
			port_lookup[p_link_tbl[i].from_port_num] = to_port_index ;
		} else {
			p_index->ca_link_lookup[ntohs(p_link_tbl[i].from_lid)] = to_port_index;
		}
	}

	return 0;
}

int ssa_pr_build_indexes(struct ssa_pr_smdb_index *p_index,
			 const struct ssa_db *p_smdb)
{
	int res = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);

	res = build_is_switch_lookup(p_index, p_smdb);
	if (res) {
		SSA_PR_LOG_ERROR("Build for is_switch_lookup failed");
		return res;
	}
	res = build_port_index(p_index, p_smdb);
	if (res) {
		SSA_PR_LOG_ERROR("Build for port index failed");
		return res;
	}
	res = build_lft_top_lookup(p_index, p_smdb);
	if (res) {
		SSA_PR_LOG_ERROR("Build for lft_top failed");
		return res;
	}
	res = build_lft_block_lookup(p_index, p_smdb);
	if (res) {
		SSA_PR_LOG_ERROR("Build for lft block lookup failed");
		return res;
	}
	res = build_link_index(p_index, p_smdb);
	if (res) {
		SSA_PR_LOG_ERROR("Build for link index failed");
		return res;
	}

	p_index->epoch = ssa_db_get_epoch(p_smdb, DB_DEF_TBL_ID);

	return 0;
}

void ssa_pr_destroy_indexes(struct ssa_pr_smdb_index *p_index)
{
	size_t i = 0;

	SSA_ASSERT(p_index);

	memset(p_index->is_switch_lookup, '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->is_switch_lookup[0]));
	memset(p_index->lft_top_lookup , '\0',
	       (MAX_LOOKUP_LID + 1) * sizeof(p_index->lft_top_lookup[0]));
	memset(p_index->ca_port_lookup, '\0',
	       (MAX_LOOKUP_LID +1) * sizeof(p_index->ca_port_lookup[0]));

	for (i = 0; i <= MAX_LOOKUP_LID; ++i) {
		free(p_index->switch_port_lookup[i]);
		p_index->switch_port_lookup[i] = NULL;
	}

	memset(p_index->ca_link_lookup, '\0',
	       (MAX_LOOKUP_LID +1) * sizeof(p_index->ca_link_lookup[0]));

	for (i = 0; i <= MAX_LOOKUP_LID; ++i) {
		free(p_index->switch_link_lookup[i]);
		p_index->switch_link_lookup[i] = NULL;
	}

	for (i = 0; i <= MAX_LOOKUP_LID; ++i) {
		if (p_index->lft_block_lookup[i]) {
			free(p_index->lft_block_lookup[i]);
			p_index->lft_block_lookup[i] = NULL;
		}
	}

	p_index->epoch = DB_EPOCH_INVALID;
}

int ssa_pr_rebuild_indexes(struct ssa_pr_smdb_index *p_index,
			   const struct ssa_db *p_smdb)
{
	uint64_t smdb_epoch = DB_EPOCH_INVALID;
	int res = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);

	smdb_epoch = ssa_db_get_epoch(p_smdb, DB_DEF_TBL_ID);

	if (p_index->epoch != smdb_epoch) {
		ssa_pr_destroy_indexes(p_index);
		res = ssa_pr_build_indexes(p_index, p_smdb);
		if (res) {
			SSA_PR_LOG_ERROR("SMDB index creation failed. epoch: 0x%" PRIx64,
					 smdb_epoch);
			return res;
		}
		p_index->epoch = smdb_epoch;
		SSA_PR_LOG_INFO("SMDB index created. epoch: 0x%" PRIx64,
				p_index->epoch);
	}
	return 0;
}

const struct smdb_guid2lid
*find_guid_to_lid_rec_by_guid(const struct ssa_db *p_smdb,
			      const be64_t port_guid)
{
	size_t i = 0;
	const struct smdb_guid2lid *p_guid2lid_tbl = NULL;
	size_t count = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(port_guid);

	p_guid2lid_tbl = (struct smdb_guid2lid *)p_smdb->pp_tables[SMDB_TBL_ID_GUID2LID];
	SSA_ASSERT(p_guid2lid_tbl);

	count = get_dataset_count(p_smdb, SMDB_TBL_ID_GUID2LID);

	for (i = 0; i < count; i++) {
		if (port_guid == p_guid2lid_tbl[i].guid)
			return p_guid2lid_tbl + i;
	}

	SSA_PR_LOG_ERROR("GUID to LID record not found. GUID: 0x%016" PRIx64,
			 ntohll(port_guid));

	return NULL;
}

int find_destination_port(const struct ssa_db *p_smdb,
			  const struct ssa_pr_smdb_index *p_index,
			  const be16_t source_lid, const be16_t dest_lid)
{
	struct smdb_lft_block *p_lft_block_tbl = NULL;
	size_t lft_block_count = 0;
	size_t lft_block_num = 0;
	size_t lft_port_shift = 0;
	size_t lft_block_index = 0;
	uint16_t lft_top = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);
	SSA_ASSERT(source_lid);
	SSA_ASSERT(dest_lid);

	p_lft_block_tbl = (struct smdb_lft_block *)p_smdb->pp_tables[SMDB_TBL_ID_LFT_BLOCK];
	SSA_ASSERT(p_lft_block_tbl);

	lft_block_count = get_dataset_count(p_smdb, SMDB_TBL_ID_LFT_BLOCK);

	/*
	 * Optimization. If UMAD_LEN_SMP_DATA is power of 2, we can use shift istead of division
	 *
	 * lft_block_num = floorl(ntohs(dest_lid) / UMAD_LEN_SMP_DATA);
	 */
	lft_block_num = ntohs(dest_lid) >> 6;
	lft_port_shift = ntohs(dest_lid) % UMAD_LEN_SMP_DATA;
	lft_top = p_index->lft_top_lookup[ntohs(source_lid)];

	if (ntohs(dest_lid) > lft_top) {
		SSA_PR_LOG_ERROR("LFT routing failed. Destination LID exceeds LFT top. "
				 "Source LID (%u) Destination LID: (%u) LFT top: %u",
				 ntohs(source_lid), ntohs(dest_lid), lft_top);
		return -1;
	}

	if (!p_index->lft_block_lookup[ntohs(source_lid)] ||
	    p_index->lft_block_lookup[ntohs(source_lid)][lft_block_num] > lft_block_count) {
		SSA_PR_LOG_ERROR("LFT routing failed. Destination LID exceeds LFT top. "
				 "Source LID (%u) Destination LID: (%u) LFT top: %u",
				 ntohs(source_lid), ntohs(dest_lid), lft_top);
		return -1;
	}

	lft_block_index = p_index->lft_block_lookup[ntohs(source_lid)][lft_block_num];
	return p_lft_block_tbl[lft_block_index].block[lft_port_shift];
}

static size_t find_port_index(const struct ssa_pr_smdb_index *p_index,
			      const be16_t lid, const unsigned int port_num)
{
	size_t port_index = -1;

	SSA_ASSERT(p_index);
	SSA_ASSERT(p_index->is_switch_lookup);
	SSA_ASSERT(lid);

	if (p_index->is_switch_lookup[ntohs(lid)]) {
		uint64_t *switch_port_lookup = p_index->switch_port_lookup[ntohs(lid)];

		if (!switch_port_lookup) {
			SSA_PR_LOG_ERROR("Port not found. LID: %u Port num: %u",
					 ntohs(lid), port_num);
			return -1;
		}
		port_index = switch_port_lookup[port_num];
	} else {
		port_index = p_index->ca_port_lookup[ntohs(lid)];
	}

	return port_index;
}

const struct smdb_port *find_port(const struct ssa_db *p_smdb,
				  const struct ssa_pr_smdb_index *p_index,
				  const be16_t lid, const int port_num)
{
	size_t port_index = 0;
	const struct smdb_port *p_port_tbl = NULL;
	size_t count = 0;

	p_port_tbl =
		(const struct smdb_port *)p_smdb->pp_tables[SMDB_TBL_ID_PORT];
	SSA_ASSERT(p_port_tbl);

	count = get_dataset_count(p_smdb, SMDB_TBL_ID_PORT);

	port_index = find_port_index(p_index, lid, port_num);

	if (port_index >= count) {
		SSA_PR_LOG_ERROR("Port not found. LID: %u Port num: %d",
				 ntohs(lid), port_num);
		return NULL;
	}
	return p_port_tbl + port_index;
}

const struct smdb_port *find_linked_port(const struct ssa_db *p_smdb,
					 const struct ssa_pr_smdb_index *p_index,
					 const be16_t lid, const int port_num)
{
	const struct smdb_port *p_port_tbl = NULL;
	size_t port_count = 0;
	size_t record_index = 0;

	SSA_ASSERT(p_smdb);
	SSA_ASSERT(p_index);
	SSA_ASSERT(p_index->is_switch_lookup);
	SSA_ASSERT(lid);

	p_port_tbl = (const struct smdb_port *)p_smdb->pp_tables[SMDB_TBL_ID_PORT];
	SSA_ASSERT(p_port_tbl);

	if (p_index->is_switch_lookup[ntohs(lid)]) {
		uint64_t *port_lookup = p_index->switch_link_lookup[ntohs(lid)];
		if (!port_lookup) {
			SSA_PR_LOG_ERROR("Link not found. LID: %u Port num: %u",
					 ntohs(lid), port_num);
			return NULL;
		}
		record_index = port_lookup[port_num];
	} else {
		record_index = p_index->ca_link_lookup[ntohs(lid)];
	}

	port_count = get_dataset_count(p_smdb, SMDB_TBL_ID_PORT);

	if (record_index >= port_count) {
		if (port_num >= 0) {
			SSA_PR_LOG_ERROR("Link not found. LID: %u Port num: %u",
					 ntohs(lid), port_num);
		} else {
			SSA_PR_LOG_ERROR("Link not found. LID: %u", ntohs(lid));
		}
		return NULL;
	}

	return p_port_tbl + record_index;
}

int is_port_exist(const struct ssa_db *p_smdb, be64_t guid)
{
	size_t i = 0, count = 0;
	const struct smdb_guid2lid *p_guid2lid_tbl = NULL;

	SSA_ASSERT(p_smdb);

	p_guid2lid_tbl =
		(struct smdb_guid2lid *)p_smdb->pp_tables[SMDB_TBL_ID_GUID2LID];
	SSA_ASSERT(p_guid2lid_tbl);

	count = get_dataset_count(p_smdb, SMDB_TBL_ID_GUID2LID);
	if (!count) {
		SSA_PR_LOG_INFO("Guid to LID table is empty");
		return 0;
	}

	for (i = 0; i < count; i++)
		if (p_guid2lid_tbl[i].guid == guid)
			return 1;

	return 0;
}
