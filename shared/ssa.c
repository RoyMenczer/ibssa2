/*
 * Copyright (c) 2013 Mellanox Technologies LTD. All rights reserved.
 * Copyright (c) 2013 Intel Corporation. All rights reserved.
 * Copyright (c) 2013 Lawrence Livermore National Securities.  All rights reserved.
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

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <osd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <rdma/rsocket.h>
#include <syslog.h>
#include <netinet/tcp.h>
#include <infiniband/umad.h>
#include <infiniband/umad_str.h>
#include <infiniband/verbs.h>
#include <infiniband/ssa.h>
#include <infiniband/ib.h>
#include <infiniband/ssa_db.h>
#include <infiniband/ssa_path_record.h>
#include <infiniband/ssa_db_helper.h>
#include <dlist.h>
#include <search.h>
#include <common.h>
#include <ssa_ctrl.h>


#define DEFAULT_TIMEOUT 1000
#define MAX_TIMEOUT	120 * DEFAULT_TIMEOUT

#define FIRST_DATA_FD_SLOT	6

#define SMDB_PRELOAD_PATH RDMA_CONF_DIR "/smdb"
#define PRDB_PRELOAD_PATH RDMA_CONF_DIR "/prdb"

struct ssa_access_context {
	struct ssa_db *smdb;
	void *context;
};

#ifdef ACCESS_INTEGRATION
static struct ssa_db *prdb;
#endif
static struct ssa_db *smdb;
static FILE *flog;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ssa_access_context access_context;

__thread char log_data[128];
//static atomic_t counter[SSA_MAX_COUNTER];

static const char * month_str[] = {
	"Jan",
	"Feb",
	"Mar",
	"Apr",
	"May",
	"Jun",
	"Jul",
	"Aug",
	"Sep",
	"Oct",
	"Nov",
	"Dec"
};

static int log_level = SSA_LOG_DEFAULT;
//static short server_port = 6125;
short smdb_port = 7470;
short prdb_port = 7471;

/* Forward declarations */
static void ssa_close_ssa_conn(struct ssa_conn *conn);
static int ssa_downstream_svc_server(struct ssa_svc *svc, struct ssa_conn *conn);
static int ssa_upstream_initiate_conn(struct ssa_svc *svc, short dport);
static void ssa_upstream_svc_client(struct ssa_svc *svc, int errnum);

void ssa_set_log_level(int level)
{
	log_level = level;
}

int ssa_open_log(char *log_file)
{
	if (!strcasecmp(log_file, "stdout")) {
		flog = stdout;
		return 0;
	}

	if (!strcasecmp(log_file, "stderr")) {
		flog = stderr;
		return 0;
	}

	if ((flog = fopen(log_file, "w")))
		return 0;

	syslog(LOG_WARNING, "Failed to open log file %s\n", log_file);
	flog = stderr;
	return -1;
}

void ssa_close_log()
{
	fclose(flog);
}

void ssa_write_log(int level, const char *format, ...)
{
	va_list args;
	pid_t tid;
	struct timeval tv;
	time_t tim;
	struct tm result;

	if (!(level & log_level))
		return;

	gettimeofday(&tv, NULL);
	tim = tv.tv_sec;
	localtime_r(&tim, &result);
	tid = pthread_self();
	va_start(args, format);
	pthread_mutex_lock(&log_lock);
	fprintf(flog, "%s %02d %02d:%02d:%02d %06d [%04X]: ",
		(result.tm_mon < 12 ? month_str[result.tm_mon] : "???"),
		result.tm_mday, result.tm_hour, result.tm_min,
		result.tm_sec, (unsigned int)tv.tv_usec, tid);
	vfprintf(flog, format, args);
	fflush(flog);
	pthread_mutex_unlock(&log_lock);
	va_end(args);
}

void ssa_sprint_addr(int level, char *str, size_t str_size,
		     enum ssa_addr_type addr_type, uint8_t *addr, size_t addr_size)
{
	struct ibv_path_record *path;

	if (!(level & log_level))
		return;

	switch (addr_type) {
	case SSA_ADDR_NAME:
		memcpy(str, addr, addr_size);
		break;
	case SSA_ADDR_IP:
		inet_ntop(AF_INET, addr, str, str_size);
		break;
	case SSA_ADDR_IP6:
	case SSA_ADDR_GID:
		inet_ntop(AF_INET6, addr, str, str_size);
		break;
	case SSA_ADDR_PATH:
		path = (struct ibv_path_record *) addr;
		if (path->dlid) {
			snprintf(str, str_size, "SLID(%u) DLID(%u)",
				ntohs(path->slid), ntohs(path->dlid));
		} else {
			ssa_sprint_addr(level, str, str_size, SSA_ADDR_GID,
					path->dgid.raw, sizeof path->dgid);
		}
		break;
	case SSA_ADDR_LID:
		snprintf(str, str_size, "LID(%u)", ntohs(*((uint16_t *) addr)));
		break;
	default:
		strcpy(str, "Unknown");
		break;
	}
}

void ssa_log_options()
{
	ssa_log(SSA_LOG_DEFAULT, "log level 0x%x\n", log_level);
}

const char *ssa_method_str(uint8_t method)
{
	return umad_method_str(UMAD_CLASS_SUBN_ADM, method);
}

const char *ssa_attribute_str(be16_t attr_id)
{
	switch  (ntohs(attr_id)) {
	case SSA_ATTR_MEMBER_REC:
		return "MemberRecord";
	case SSA_ATTR_INFO_REC:
		return "InfoRecord";
	default:
		return umad_attribute_str(UMAD_CLASS_SUBN_ADM, attr_id);
	}
}

const char *ssa_mad_status_str(be16_t status)
{
	return umad_sa_mad_status_str(status);
}

int ssa_compare_gid(const void *gid1, const void *gid2)
{
	return memcmp(gid1, gid2, 16);
}

static be64_t ssa_svc_tid(struct ssa_svc *svc)
{
	return htonll((((uint64_t) svc->index) << 16) | svc->tid++);
}

static struct ssa_svc *ssa_svc_from_tid(struct ssa_port *port, be64_t tid)
{
	uint16_t index = (uint16_t) (ntohll(tid) >> 16);
	return (index < port->svc_cnt) ? port->svc[index] : NULL;
}

static struct ssa_svc *ssa_find_svc(struct ssa_port *port, uint64_t database_id)
{
	int i;
	for (i = 0; i < port->svc_cnt; i++) {
		if (port->svc[i] && port->svc[i]->database_id == database_id)
			return port->svc[i];
	}
	return NULL;
}

void ssa_init_mad_hdr(struct ssa_svc *svc, struct umad_hdr *hdr,
		      uint8_t method, uint16_t attr_id)
{
	hdr->base_version = UMAD_BASE_VERSION;
	hdr->mgmt_class = SSA_CLASS;
	hdr->class_version = SSA_CLASS_VERSION;
	hdr->method = method;
	hdr->tid = ssa_svc_tid(svc);
	hdr->attr_id = htons(attr_id);
}

static void sa_init_mad_hdr(struct ssa_svc *svc, struct umad_hdr *hdr,
			    uint8_t method, uint16_t attr_id)
{
	hdr->base_version = UMAD_BASE_VERSION;
	hdr->mgmt_class = UMAD_CLASS_SUBN_ADM;
	hdr->class_version = UMAD_SA_CLASS_VERSION;
	hdr->method = method;
	hdr->tid = ssa_svc_tid(svc);
	hdr->attr_id = htons(attr_id);
}

static void ssa_init_join(struct ssa_svc *svc, struct ssa_mad_packet *mad)
{
	struct ssa_member_record *rec;

	ssa_init_mad_hdr(svc, &mad->mad_hdr, UMAD_METHOD_SET, SSA_ATTR_MEMBER_REC);
	mad->ssa_key = 0;	/* TODO: set for real */

	rec = (struct ssa_member_record *) &mad->data;
	memcpy(rec->port_gid, svc->port->gid.raw, 16);
	rec->database_id = htonll(svc->database_id);
	rec->node_guid = svc->port->dev->guid;
	rec->node_type = svc->port->dev->ssa->node_type;
}

static void sa_init_path_query(struct ssa_svc *svc, struct umad_sa_packet *mad,
			       union ibv_gid *dgid, union ibv_gid *sgid)
{
	struct ibv_path_record *path;

	sa_init_mad_hdr(svc, &mad->mad_hdr, UMAD_METHOD_GET,
			UMAD_SA_ATTR_PATH_REC);
	mad->comp_mask = htonll(((uint64_t)1) << 2 |	/* DGID */
				((uint64_t)1) << 3 |	/* SGID */
				((uint64_t)1) << 11 |	/* Reversible */
				((uint64_t)1) << 13);	/* P_Key */

	path = (struct ibv_path_record *) &mad->data;
	memcpy(path->dgid.raw, dgid, 16);
	memcpy(path->sgid.raw, sgid, 16);
	path->reversible_numpath = IBV_PATH_RECORD_REVERSIBLE;
	path->pkey = 0xFFFF;	/* default partition */
}

static void ssa_svc_join(struct ssa_svc *svc)
{
	struct ssa_umad umad;
	int ret;

	ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, svc->port->gid.raw, sizeof svc->port->gid);
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n", svc->name, log_data);
	memset(&umad, 0, sizeof umad);
	umad_set_addr(&umad.umad, svc->port->sm_lid, 1, svc->port->sm_sl, UMAD_QKEY);
	ssa_init_join(svc, &umad.packet);
	svc->state = SSA_STATE_JOINING;

	ret = umad_send(svc->port->mad_portid, svc->port->mad_agentid,
			(void *) &umad, sizeof umad.packet, svc->timeout, 0);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "failed to send join request\n");
		svc->state = SSA_STATE_IDLE;
	}
}

static void ssa_init_ssa_msg_hdr(struct ssa_msg_hdr *hdr, uint16_t op,
				 uint32_t len, uint16_t flags, uint32_t id)
{
	hdr->version = SSA_MSG_VERSION;
	hdr->class = SSA_MSG_CLASS_DB;
	hdr->op = htons(op);
	hdr->len = htonl(len);
	hdr->flags = htons(flags);
	hdr->status = 0;
	hdr->id = htonl(id);
	hdr->reserved = 0;
	hdr->rdma_len = 0;
	hdr->rdma_addr = 0;
}

static int validate_ssa_msg_hdr(struct ssa_msg_hdr *hdr)
{
	if (hdr->version != SSA_MSG_VERSION)
		return 0;
	if (hdr->class != SSA_MSG_CLASS_DB)
		return 0;
	switch (ntohs(hdr->op)) {
	case SSA_MSG_DB_QUERY_DEF:
	case SSA_MSG_DB_QUERY_TBL_DEF:
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
	case SSA_MSG_DB_QUERY_DATA_DATASET:
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
		return 1;
	default:
		return 0;
	}
}

static int ssa_downstream_listen(struct ssa_svc *svc,
				 struct ssa_conn *conn_listen, short sport)
{
	struct sockaddr_ib src_addr;
	int ret, val;

	/* Only listening on rsocket when server (not consumer - ACM) */
	if (svc->port->dev->ssa->node_type == SSA_NODE_CONSUMER)
		return -1;

	if (conn_listen->rsock >= 0)
		return conn_listen->rsock;

	ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL, "%s\n", svc->port->name);

	conn_listen->rsock = rsocket(AF_IB, SOCK_STREAM, 0);
	if (conn_listen->rsock < 0) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsocket ERROR %d (%s)\n",
			errno, strerror(errno));
		return -1;
	}

	val = 1;
	ret = rsetsockopt(conn_listen->rsock, SOL_SOCKET, SO_REUSEADDR,
			  &val, sizeof val);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt SO_REUSEADDR ERROR %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}

	ret = rsetsockopt(conn_listen->rsock, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt TCP_NODELAY ERROR %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}
	ret = rfcntl(conn_listen->rsock, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rfcntl ERROR %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}

	src_addr.sib_family = AF_IB;
	src_addr.sib_pkey = 0xFFFF;
	src_addr.sib_flowinfo = 0;
	src_addr.sib_sid = htonll(((uint64_t) RDMA_PS_TCP << 16) + sport);
	src_addr.sib_sid_mask = htonll(RDMA_IB_IP_PS_MASK | RDMA_IB_IP_PORT_MASK);
	src_addr.sib_scope_id = 0;
	memcpy(&src_addr.sib_addr, &svc->port->gid, 16);

	ret = rbind(conn_listen->rsock, (const struct sockaddr *) &src_addr,
		    sizeof(src_addr));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rbind ERROR %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}
	ret = rlisten(conn_listen->rsock, 1);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rlisten ERROR %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}
	conn_listen->state = SSA_CONN_LISTENING;

	return conn_listen->rsock;

err:
	ssa_close_ssa_conn(conn_listen);
	return -1;
}

void ssa_svc_query_path(struct ssa_svc *svc, union ibv_gid *dgid,
			union ibv_gid *sgid)
{
	struct sa_umad umad;
	int ret;

	memset(&umad, 0, sizeof umad);
	umad_set_addr(&umad.umad, svc->port->sm_lid, 1, svc->port->sm_sl, UMAD_QKEY);
	sa_init_path_query(svc, &umad.packet, dgid, sgid);

	ret = umad_send(svc->port->mad_portid, svc->port->mad_agentid,
			(void *) &umad, sizeof umad.packet, svc->timeout, 0);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "failed to send path query to SA\n");
        }
}

static void ssa_upstream_dev_event(struct ssa_svc *svc, struct ssa_ctrl_msg_buf *msg)
{
	int i;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s %s\n", svc->name,
		ibv_event_type_str(msg->data.event));
	switch (msg->data.event) {
	case IBV_EVENT_CLIENT_REREGISTER:
	case IBV_EVENT_PORT_ERR:
		if (svc->conn_listen_smdb.rsock >= 0)
			ssa_close_ssa_conn(&svc->conn_listen_smdb);
		if (svc->conn_listen_prdb.rsock >= 0)
			ssa_close_ssa_conn(&svc->conn_listen_prdb);
		if (svc->conn_dataup.rsock >= 0)
			ssa_close_ssa_conn(&svc->conn_dataup);
		if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
			for (i = 0; i < FD_SETSIZE; i++) {
				if (svc->fd_to_conn[i] &&
				    svc->fd_to_conn[i]->rsock >= 0) {
					ssa_close_ssa_conn(svc->fd_to_conn[i]);
					svc->fd_to_conn[i] = NULL;
				}
			}
		}
		svc->state = SSA_STATE_IDLE;
		/* fall through to reactivate */
	case IBV_EVENT_PORT_ACTIVE:
		if (svc->port->state == IBV_PORT_ACTIVE && svc->state == SSA_STATE_IDLE) {
			svc->timeout = DEFAULT_TIMEOUT;
			ssa_svc_join(svc);
		}
		break;
	default:
		break;
	}
}

void ssa_upstream_mad(struct ssa_svc *svc, struct ssa_ctrl_msg_buf *msg)
{
	struct ssa_umad *umad;
	struct ssa_mad_packet *mad;
	struct ssa_info_record *info_rec;

	umad = &msg->data.umad;
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	if (svc->state == SSA_STATE_IDLE) {
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "in idle state, discarding MAD\n");
		svc->timeout = DEFAULT_TIMEOUT;
		return;
	}

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "method %s attr %s\n",
		ssa_method_str(umad->packet.mad_hdr.method),
		ssa_attribute_str(umad->packet.mad_hdr.attr_id));
	/* TODO: do we need to check umad->packet.mad_hdr.status too? */
	if (umad->umad.status) {
		ssa_log(SSA_LOG_DEFAULT, "send failed - status 0x%x (%s)\n",
			umad->umad.status, strerror(umad->umad.status));
		if (svc->state != SSA_STATE_JOINING)
			return;

		svc->timeout = min(svc->timeout << 1, MAX_TIMEOUT);
		ssa_svc_join(svc);
		return;
	}

	svc->timeout = DEFAULT_TIMEOUT;
	if (svc->state == SSA_STATE_JOINING) {
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "join successful\n");
		svc->state = SSA_STATE_ORPHAN;
	}

	if (ntohs(umad->packet.mad_hdr.attr_id) != SSA_ATTR_INFO_REC)
		return;

	umad->packet.mad_hdr.method = UMAD_METHOD_GET_RESP;
	umad_send(svc->port->mad_portid, svc->port->mad_agentid,
		  (void *) umad, sizeof umad->packet, 0, 0);

	switch (svc->state) {
	case SSA_STATE_ORPHAN:
		svc->state = SSA_STATE_HAVE_PARENT;
	case SSA_STATE_HAVE_PARENT:
		mad = &umad->packet;
		info_rec = (struct ssa_info_record *) &mad->data;
		memcpy(&svc->primary_parent, &info_rec->path_data,
		       sizeof(svc->primary_parent));
		break;
	case SSA_STATE_CONNECTING:
	case SSA_STATE_CONNECTED:		/* TODO compare against current parent, if same done */
		/* if parent is different, save parent, close rsock, and reopen */
		break;
	default:
		break;
	}
}

static void ssa_init_ssa_conn(struct ssa_conn *conn, int conn_type,
			      int conn_dbtype)
{
	conn->rsock = -1;
	conn->type = conn_type;
	conn->dbtype = conn_dbtype;
	conn->state = SSA_CONN_IDLE;
	conn->phase = SSA_DB_IDLE;
	conn->rbuf = NULL;
	conn->rid = 0;
	conn->rindex = 0;
	conn->rhdr = NULL;
	conn->sbuf = NULL;
	conn->sid = 0;
	conn->sindex = 0;
	conn->sbuf2 = NULL;
	conn->ssa_db = NULL;
}

static void ssa_close_ssa_conn(struct ssa_conn *conn)
{
	if (!conn)
		return;
	rclose(conn->rsock);
	conn->rsock = -1;
	conn->dbtype = SSA_CONN_NODB_TYPE;
	conn->state = SSA_CONN_IDLE;
}

static int ssa_upstream_send_query(int rsock, struct ssa_msg_hdr *msg,
				   uint16_t op, uint32_t id)
{
	ssa_init_ssa_msg_hdr(msg, op, sizeof(*msg), SSA_MSG_FLAG_END, id);
	return rsend(rsock, msg, sizeof(*msg), MSG_DONTWAIT);
}

static void ssa_upstream_update_phase(struct ssa_conn *conn, uint16_t op)
{
	switch (op) {
	case SSA_MSG_DB_QUERY_DEF:
		conn->phase = SSA_DB_DEFS;
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF:
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
		conn->phase = SSA_DB_TBL_DEFS;
		break;
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
		conn->phase = SSA_DB_FIELD_DEFS;
		break;
	case SSA_MSG_DB_QUERY_DATA_DATASET:
		conn->phase = SSA_DB_DATA;
		break;
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
		ssa_log_warn(SSA_LOG_CTRL,
			     "SSA_MSG_DB_PUBLISH_EPOCH_BUF not currently supported\n");
		break;
	default:
		ssa_log_warn(SSA_LOG_CTRL, "unknown op %u\n", op);
		break;
	}
}

static short ssa_upstream_query(struct ssa_svc *svc, uint16_t op, short events)
{
	uint32_t id;
	int ret;

	svc->conn_dataup.sbuf = malloc(sizeof(struct ssa_msg_hdr));
	if (svc->conn_dataup.sbuf) {
		svc->conn_dataup.ssize = sizeof(struct ssa_msg_hdr);
		svc->conn_dataup.soffset = 0;
		id = svc->tid++;

		ret = ssa_upstream_send_query(svc->conn_dataup.rsock,
					      svc->conn_dataup.sbuf, op, id);
		if (ret > 0) {
			ssa_upstream_update_phase(&svc->conn_dataup, op);
			svc->conn_dataup.soffset += ret;
			svc->conn_dataup.sid = id;
			if (svc->conn_dataup.soffset == svc->conn_dataup.ssize) {
				free(svc->conn_dataup.sbuf);
				svc->conn_dataup.sbuf = NULL;
				return POLLIN;
			} else {
				return POLLOUT | POLLIN;
			}
		} else {
			ssa_log_err(SSA_LOG_CTRL,
				    "ssa_upstream_send_query for op %u failed\n",
				    op);
			return 0;
		}
	} else
		ssa_log_err(SSA_LOG_CTRL,
			    "failed to allocate ssa_msg_hdr for ssa_upstream_send_query for op %u\n",
			    op);
	return events;
}

static short ssa_rsend_continue(struct ssa_conn *conn, short events)
{
	int ret;

	ret = rsend(conn->rsock, conn->sbuf + conn->soffset,
		    conn->ssize - conn->soffset, MSG_DONTWAIT);
	if (ret > 0) {
		conn->soffset += ret;
		if (conn->soffset == conn->ssize) {
			if (conn->sbuf != conn->sbuf2) {
				free(conn->sbuf);
				if (!conn->sbuf2) {
					conn->sbuf = NULL;
					return POLLIN;
				} else {
					conn->sbuf = conn->sbuf2;
					conn->ssize = conn->ssize2;
					conn->soffset = 0;
					ret = rsend(conn->rsock, conn->sbuf,
						    conn->ssize, MSG_DONTWAIT);
					if (ret > 0) {
						conn->soffset += ret;
						if (conn->soffset == conn->ssize) {
							conn->sbuf2 = NULL;
							return POLLIN;
						} else
							return POLLOUT | POLLIN;
					}
				}
			} else {
				conn->sbuf2 = NULL;
				return POLLIN;
			}
		} else {
			return POLLOUT | POLLIN;
		}
	} else {
		ssa_log_err(SSA_LOG_CTRL, "rsend continuation failed\n");
		return 0;
	}

	return events;
}

static void ssa_upstream_handle_query_defs(struct ssa_conn *conn,
					   struct ssa_msg_hdr *hdr)
{
	int ret, size;

	if (conn->phase == SSA_DB_DEFS) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_DEF/TBL_DEF ids 0x%x 0x%x don't match\n",
				conn->sid, ntohl(hdr->id));
		} else {
			conn->rhdr = hdr;
			if (conn->rindex)
				size = sizeof(struct db_dataset);
			else
				size = sizeof(struct db_def);
			if (ntohl(hdr->len) != sizeof(*hdr) + size)
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_MSG_DB_QUERY_DEF/TBL_DEF response length %d is not the expected length %d\n",
					ntohl(hdr->len), sizeof(*hdr) + size);
			else {
				if (conn->rindex)
					conn->rbuf = &conn->ssa_db->db_table_def;
				else
					conn->rbuf = &conn->ssa_db->db_def;
				conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
				conn->roffset = 0;
				ret = rrecv(conn->rsock, conn->rbuf,
					    conn->rsize, MSG_DONTWAIT);
				if (ret > 0) {
					conn->roffset += ret;
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_DEF phase %d not SSA_DB_DEFS\n",
			conn->phase);
}

static void ssa_upstream_handle_query_tbl_defs(struct ssa_conn *conn,
					       struct ssa_msg_hdr *hdr)
{
	void *buf;
	int ret;

	if (conn->phase == SSA_DB_TBL_DEFS) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_TBL_DEF ids 0x%x 0x%x don't match\n",
				conn->sid, ntohl(hdr->id));
		} else {
			conn->rhdr = hdr;
			if (ntohl(hdr->len) > sizeof(*hdr)) {
				buf = malloc(ntohl(hdr->len) - sizeof(*hdr));
				if (!buf)
					ssa_log(SSA_LOG_DEFAULT,
						"no rrecv buffer available\n");
				else {
					conn->rbuf = buf;
					conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
					conn->roffset = 0;
					ret = rrecv(conn->rsock, conn->rbuf,
						    conn->rsize, MSG_DONTWAIT);
					if (ret > 0) {
						conn->roffset += ret;
					}
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_TBL_DEF phase %d not SSA_DB_TBL_DEFS\n",
			conn->phase);
}

static void ssa_upstream_handle_query_field_defs(struct ssa_conn *conn,
						 struct ssa_msg_hdr *hdr)
{
	void *buf;
	int ret;

	if (conn->phase == SSA_DB_FIELD_DEFS) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_FIELD_DEF ids 0x%x 0x%x don't match\n",
				conn->sid, ntohl(hdr->id));
		} else {
			conn->rhdr = hdr;
			if (ntohl(hdr->len) > sizeof(*hdr)) {
				buf = malloc(ntohl(hdr->len) - sizeof(*hdr));
				if (!buf)
					ssa_log(SSA_LOG_DEFAULT,
						"no rrecv buffer available\n");
				else {
					conn->rbuf = buf;
					conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
					conn->roffset = 0;
					ret = rrecv(conn->rsock, conn->rbuf,
						    conn->rsize, MSG_DONTWAIT);
					if (ret > 0) {
						conn->roffset += ret;
					}
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_FIELD_DEF phase %d not SSA_DB_FIELD_DEFS\n",
			conn->phase);
}

static void ssa_upstream_handle_query_data(struct ssa_conn *conn,
					   struct ssa_msg_hdr *hdr)
{
	void *buf;
	int ret;

	if (conn->phase == SSA_DB_DATA) {
		if (conn->sid != ntohl(hdr->id)) {
			ssa_log(SSA_LOG_DEFAULT,
				"SSA_MSG_DB_QUERY_DATA_DATASET ids 0x%x 0x%x don't match\n",
				conn->sid, ntohl(hdr->id));
		} else {
			conn->rhdr = hdr;
			if (ntohl(hdr->len) > sizeof(*hdr)) {
				buf = malloc(ntohl(hdr->len) - sizeof(*hdr));
				if (!buf)
					ssa_log(SSA_LOG_DEFAULT,
						"no rrecv buffer available\n");
				else {
					conn->rbuf = buf;
					conn->rsize = ntohl(hdr->len) - sizeof(*hdr);
					conn->roffset = 0;
					ret = rrecv(conn->rsock, conn->rbuf,
						    conn->rsize, MSG_DONTWAIT);
					if (ret > 0) {
						conn->roffset += ret;
					}
				}
			}
		}
	} else
		ssa_log(SSA_LOG_DEFAULT,
			"SSA_MSG_DB_QUERY_DATA_DATASET phase %d not SSA_DB_DATA\n",
			conn->phase);
}

static void ssa_upstream_send_db_update(struct ssa_svc *svc, struct ssa_db *db,
					int flags, union ibv_gid *gid)
{
	struct ssa_db_update_msg msg;

	msg.hdr.type = SSA_DB_UPDATE;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = db;
	msg.db_upd.flags = flags;
	msg.db_upd.remote_gid = gid;
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS)
		write(svc->sock_accessup[0], (char *) &msg, sizeof(msg));
	if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION)
		write(svc->sock_updown[0], (char *) &msg, sizeof(msg));
	if (svc->process_msg)
		svc->process_msg(svc, (struct ssa_ctrl_msg_buf *) &msg);
}

static short ssa_upstream_update_conn(struct ssa_svc *svc, short events)
{
	uint64_t data_tbl_cnt;
	short revents = events;

	switch (svc->conn_dataup.phase) {
	case SSA_DB_IDLE:
		/* Temporary workaround !!! */
		usleep(10000);		/* 10 msec */
		revents = ssa_upstream_query(svc, SSA_MSG_DB_QUERY_DEF, events);
		svc->conn_dataup.rindex = 0;
		break;
	case SSA_DB_DEFS:
		if (svc->conn_dataup.rindex)
			svc->conn_dataup.phase = SSA_DB_TBL_DEFS;
		svc->conn_dataup.roffset = 0;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		revents = ssa_upstream_query(svc,
					     svc->conn_dataup.rindex == 0 ?
					     SSA_MSG_DB_QUERY_TBL_DEF :
					     SSA_MSG_DB_QUERY_TBL_DEF_DATASET,
					     events);
		if (svc->conn_dataup.phase == SSA_DB_DEFS)
			svc->conn_dataup.rindex++;
		else
			svc->conn_dataup.rindex = 0;
		break;
	case SSA_DB_TBL_DEFS:
		svc->conn_dataup.phase = SSA_DB_FIELD_DEFS;
		svc->conn_dataup.roffset = 0;
		svc->conn_dataup.ssa_db->p_def_tbl = svc->conn_dataup.rbuf;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		revents = ssa_upstream_query(svc,
					     SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
					     events);
		break;
	case SSA_DB_FIELD_DEFS:
		if (svc->conn_dataup.rbuf == svc->conn_dataup.rhdr &&
		    ntohs(((struct ssa_msg_hdr *)svc->conn_dataup.rhdr)->flags) & SSA_MSG_FLAG_END) {
			svc->conn_dataup.phase = SSA_DB_DATA;
		} else {
			if (!svc->conn_dataup.ssa_db->p_db_field_tables) {
				svc->conn_dataup.ssa_db->p_db_field_tables = svc->conn_dataup.rbuf;
				data_tbl_cnt = ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db);
				svc->conn_dataup.ssa_db->pp_field_tables = malloc(data_tbl_cnt * sizeof(*svc->conn_dataup.ssa_db->pp_field_tables));
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_FIELD_DEFS ssa_db allocated pp_field_tables %p num tables %d\n", svc->conn_dataup.ssa_db->pp_field_tables, data_tbl_cnt);
				svc->conn_dataup.rindex = 0;
			} else {
				if (svc->conn_dataup.ssa_db->pp_field_tables)
					svc->conn_dataup.ssa_db->pp_field_tables[svc->conn_dataup.rindex] = svc->conn_dataup.rbuf;
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_FIELD_DEFS index %d %p\n", svc->conn_dataup.rindex, svc->conn_dataup.rbuf);
				svc->conn_dataup.rindex++;
			}
		}
		svc->conn_dataup.roffset = 0;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		revents = ssa_upstream_query(svc,
					     svc->conn_dataup.phase == SSA_DB_DATA ?
					     SSA_MSG_DB_QUERY_DATA_DATASET :
					     SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
					     events);
		break;
	case SSA_DB_DATA:
		if (svc->conn_dataup.rbuf == svc->conn_dataup.rhdr &&
		    ntohs(((struct ssa_msg_hdr *)svc->conn_dataup.rhdr)->flags) & SSA_MSG_FLAG_END) {
			svc->conn_dataup.phase = SSA_DB_IDLE;
		} else {
			if (!svc->conn_dataup.ssa_db->p_db_tables) {
				svc->conn_dataup.ssa_db->p_db_tables = svc->conn_dataup.rbuf;
				data_tbl_cnt = ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db);
				svc->conn_dataup.ssa_db->pp_tables = malloc(data_tbl_cnt * sizeof(*svc->conn_dataup.ssa_db->pp_tables));
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_DATA ssa_db allocated pp_tables %p num tables %d\n", svc->conn_dataup.ssa_db->pp_tables, data_tbl_cnt);
				svc->conn_dataup.rindex = 0;
			} else {
				if (svc->conn_dataup.ssa_db->pp_tables)
					svc->conn_dataup.ssa_db->pp_tables[svc->conn_dataup.rindex] = svc->conn_dataup.rbuf;
ssa_log(SSA_LOG_DEFAULT, "SSA_DB_DATA index %d %p\n", svc->conn_dataup.rindex, svc->conn_dataup.rbuf);
				svc->conn_dataup.rindex++;
			}
		}
		svc->conn_dataup.roffset = 0;
		free(svc->conn_dataup.rhdr);
		svc->conn_dataup.rhdr = NULL;
		svc->conn_dataup.rbuf = NULL;
		if (svc->conn_dataup.phase == SSA_DB_DATA) {
			revents = ssa_upstream_query(svc,
						     SSA_MSG_DB_QUERY_DATA_DATASET,
						     events);
		} else {
			svc->conn_dataup.ssa_db->data_tbl_cnt = ssa_db_calculate_data_tbl_num(svc->conn_dataup.ssa_db);
ssa_log(SSA_LOG_DEFAULT, "ssa_db %p complete with num tables %d\n", svc->conn_dataup.ssa_db, svc->conn_dataup.ssa_db->data_tbl_cnt);
			ssa_upstream_send_db_update(svc, svc->conn_dataup.ssa_db,
						    0, NULL);
		}
		break;
	default:
		ssa_log(SSA_LOG_DEFAULT, "unknown phase %d\n",
			svc->conn_dataup.phase);
		break;
	}
	return revents;
}

static short ssa_upstream_handle_op(struct ssa_svc *svc,
				    struct ssa_msg_hdr *hdr, short events)
{
	uint16_t op;
	short revents = events;

	op = ntohs(hdr->op);
	if (!(ntohs(hdr->flags) & SSA_MSG_FLAG_RESP))
		ssa_log(SSA_LOG_DEFAULT,
			"Ignoring SSA_MSG_FLAG_RESP not set in op %u response in phase %d\n",
			op, svc->conn_dataup.phase);
	switch (op) {
	case SSA_MSG_DB_QUERY_DEF:
	case SSA_MSG_DB_QUERY_TBL_DEF:
		ssa_upstream_handle_query_defs(&svc->conn_dataup, hdr);
		if (svc->conn_dataup.phase == SSA_DB_DEFS) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* duplicate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_DEFS received id 0x%x expected id 0x%x\n",
					ntohl(hdr->id), svc->conn_dataup.sid);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_DEFS\n",
				svc->conn_dataup.phase);
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
		ssa_upstream_handle_query_tbl_defs(&svc->conn_dataup, hdr);
		if (svc->conn_dataup.phase == SSA_DB_TBL_DEFS) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* duplicate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_TBL_DEFS received id 0x%x expected id 0x%x\n",
					ntohl(hdr->id), svc->conn_dataup.sid);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_TBL_DEFS\n",
				svc->conn_dataup.phase);
		break;
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
		ssa_upstream_handle_query_field_defs(&svc->conn_dataup, hdr);
		if (svc->conn_dataup.phase == SSA_DB_FIELD_DEFS) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* duplicate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_FIELD_DEFS received id 0x%x expected id 0x%x\n",
					ntohl(hdr->id), svc->conn_dataup.sid);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_FIELD_DEFS\n",
				svc->conn_dataup.phase);
		break;
	case SSA_MSG_DB_QUERY_DATA_DATASET:
		ssa_upstream_handle_query_data(&svc->conn_dataup, hdr);
		if (svc->conn_dataup.phase == SSA_DB_DATA) {
			if (ntohl(hdr->id) == svc->conn_dataup.sid) {	/* dupli
cate check !!! */
				if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
					revents = ssa_upstream_update_conn(svc,
									   events);
				}
			} else
				ssa_log(SSA_LOG_DEFAULT,
					"SSA_DB_DATA received id 0x%x expected id 0x%x\n",
					ntohl(hdr->id), svc->conn_dataup.sid);
		} else
			ssa_log(SSA_LOG_DEFAULT,
				"phase %d is not SSA_DB_DATA\n",
				svc->conn_dataup.phase);
		break;
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
		ssa_log_warn(SSA_LOG_CTRL,
			     "SSA_MSG_DB_PUBLISH_EPOCH_BUF not supported yet\n");
		break;
	default:
		ssa_log_warn(SSA_LOG_CTRL, "unknown op %u\n", op);
		break;
	}
	return revents;
}

static short ssa_upstream_rrecv(struct ssa_svc *svc, short events)
{
	struct ssa_msg_hdr *hdr;
	int ret;
	uint16_t op;
	short revents = events;

	ret = rrecv(svc->conn_dataup.rsock,
		    svc->conn_dataup.rbuf + svc->conn_dataup.roffset,
		    svc->conn_dataup.rsize - svc->conn_dataup.roffset, MSG_DONTWAIT);
	if (ret > 0) {
		svc->conn_dataup.roffset += ret;
		if (svc->conn_dataup.roffset == svc->conn_dataup.rsize) {
			if (!svc->conn_dataup.rhdr) {
				hdr = svc->conn_dataup.rbuf;
				if (validate_ssa_msg_hdr(hdr)) {
					op = ntohs(hdr->op);
					if (!(ntohs(hdr->flags) & SSA_MSG_FLAG_RESP))
						ssa_log(SSA_LOG_DEFAULT,
							"Ignoring SSA_MSG_FLAG_RESP not set in op %u response in phase %d\n",
							op,
							svc->conn_dataup.phase);
					revents = ssa_upstream_handle_op(svc, hdr, events);
				} else
					ssa_log_warn(SSA_LOG_CTRL,
						     "validate_ssa_msg_hdr failed: version %d class %d op %u id 0x%x\n",
						     hdr->version, hdr->class,
						     ntohs(hdr->op),
						     ntohl(hdr->id));
			} else {
				revents = ssa_upstream_update_conn(svc, events);
			}
		}
	}
	return revents;
}

static void *ssa_upstream_handler(void *context)
{
	struct ssa_svc *svc = context;
	struct ssa_conn_req_msg *conn_req;
	struct ssa_ctrl_msg_buf msg;
	struct pollfd fds[3];
	int ret, errnum;
	short port;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	write(svc->sock_upctrl[1], (char *) &msg, sizeof msg.hdr);

	fds[0].fd = svc->sock_upctrl[1];
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	fds[1].fd = svc->sock_accessup[0];
	fds[1].events = POLLIN;
	fds[1].revents = 0;
	fds[2].fd = -1;		/* placeholder for upstream connection */
	fds[2].events = 0;
	fds[2].revents = 0;

	for (;;) {
		ret = rpoll(&fds[0], 3, -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}
		errnum = errno;
		if (fds[0].revents) {
			fds[0].revents = 0;
			read(svc->sock_upctrl[1], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_upctrl[1],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;

			switch (msg.hdr.type) {
			case SSA_CTRL_MAD:
				ssa_upstream_mad(svc, &msg);
				break;
			case SSA_CTRL_DEV_EVENT:
				ssa_upstream_dev_event(svc, &msg);
				break;
			case SSA_CONN_REQ:
				conn_req = (struct ssa_conn_req_msg *) &msg;
				if (conn_req->svc->port->dev->ssa->node_type ==
				    SSA_NODE_CONSUMER) {
					port = prdb_port;
					conn_req->svc->conn_dataup.dbtype = SSA_CONN_PRDB_TYPE;
				} else {
					conn_req->svc->conn_dataup.dbtype = SSA_CONN_SMDB_TYPE;
					port = smdb_port;
				}
				fds[2].fd = ssa_upstream_initiate_conn(conn_req->svc, port);
				/* Change when more than 1 data connection supported !!! */
				if (fds[2].fd >= 0) {
					if (conn_req->svc->conn_dataup.state != SSA_CONN_CONNECTED)
						fds[2].events = POLLOUT;
					else {
						conn_req->svc->conn_dataup.ssa_db = calloc(1, sizeof(*conn_req->svc->conn_dataup.ssa_db));
						if (conn_req->svc->conn_dataup.ssa_db) {
							fds[2].events = ssa_upstream_update_conn(conn_req->svc, fds[2].events);
						} else {
							ssa_log_err(SSA_LOG_DEFAULT,
								    "could not allocate ssa_db struct\n");
						}
					}
				}
				break;
			case SSA_CTRL_EXIT:
				goto out;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from ctrl\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[1].revents) {
			fds[1].revents = 0;
			read(svc->sock_accessup[0], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_accessup[0],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from access\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[2].revents) {
			/* Only 1 data connection right now !!! */
			if (fds[2].revents & POLLOUT) {
				/* Check connection state for fd */
				if (svc->conn_dataup.state != SSA_CONN_CONNECTED) {
					ssa_upstream_svc_client(svc, errnum);
					svc->conn_dataup.ssa_db = calloc(1, sizeof(*svc->conn_dataup.ssa_db));
					if (svc->conn_dataup.ssa_db) {
						fds[2].events = ssa_upstream_update_conn(svc, fds[2].events);
					} else {
						ssa_log_err(SSA_LOG_DEFAULT, "could not allocate ssa_db struct\n");
					}
				} else {
					fds[2].events = ssa_rsend_continue(&svc->conn_dataup, fds[2].events);
				}
			}
			if (fds[2].revents & POLLIN) {
				if (!svc->conn_dataup.rbuf) {
					svc->conn_dataup.rbuf = malloc(sizeof(struct ssa_msg_hdr));
					if (svc->conn_dataup.rbuf) {
						svc->conn_dataup.rsize = sizeof(struct ssa_msg_hdr);
						svc->conn_dataup.roffset = 0;
						svc->conn_dataup.rhdr = NULL;
					} else
						ssa_log_err(SSA_LOG_CTRL,
							    "failed to allocate ssa_msg_hdr for rrecv\n");
				}
				if (svc->conn_dataup.rbuf) {
					fds[2].events = ssa_upstream_rrecv(svc, fds[2].events);
				}
			}
			if (fds[2].revents & ~(POLLOUT | POLLIN)) {
				ssa_log(SSA_LOG_DEFAULT,
					"unexpected event 0x%x on upstream rsock %d\n",
					fds[2].revents & ~(POLLOUT | POLLIN),
					fds[2].fd);
			}

			fds[2].revents = 0;
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif
		}

	}
out:
	return NULL;
}

static void ssa_downstream_conn_done(struct ssa_svc *svc, struct ssa_conn *conn)
{
	struct ssa_conn_done_msg msg;

	if (conn->dbtype != SSA_CONN_PRDB_TYPE)
		return;
	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_CONN_DONE;
	msg.hdr.len = sizeof(msg);
	msg.conn = conn;
	write(svc->sock_accessdown[0], (char *) &msg, sizeof msg);
}

static short ssa_downstream_send_resp(struct ssa_conn *conn, uint16_t op,
				      short events)
{
	int ret;

	conn->sbuf = malloc(sizeof(struct ssa_msg_hdr));
	if (conn->sbuf) {
		conn->ssize = sizeof(struct ssa_msg_hdr);
		conn->soffset = 0;
		ssa_init_ssa_msg_hdr(conn->sbuf, op, conn->ssize,
				     SSA_MSG_FLAG_END | SSA_MSG_FLAG_RESP,
				     conn->rid);
		ret = rsend(conn->rsock, conn->sbuf, conn->ssize, MSG_DONTWAIT);
		if (ret > 0) {
			conn->soffset += ret;
			if (conn->soffset == conn->ssize) {
				free(conn->sbuf);
				conn->sbuf = NULL;
				return POLLIN;
			} else
				return POLLOUT | POLLIN;
		}
	} else
		ssa_log_err(SSA_LOG_CTRL,
			    "failed to allocate ssa_msg_hdr for response to op %d\n",
			    op);
	return events;
}

static short ssa_downstream_send(struct ssa_conn *conn, uint16_t op,
				 void *buf, size_t len, short events)
{
	int ret;

	conn->sbuf = malloc(sizeof(struct ssa_msg_hdr));
	conn->sbuf2 = buf;
	if (conn->sbuf) {
		conn->ssize = sizeof(struct ssa_msg_hdr);
		conn->ssize2 = len;
		conn->soffset = 0;
		ssa_init_ssa_msg_hdr(conn->sbuf, op, conn->ssize + len,
				     SSA_MSG_FLAG_RESP, conn->rid);
		ret = rsend(conn->rsock, conn->sbuf, conn->ssize, MSG_DONTWAIT);
		if (ret > 0) {
			conn->soffset += ret;
			if (conn->soffset == conn->ssize) {
				free(conn->sbuf);
				if (!conn->sbuf2 || conn->ssize2 == 0)
					return POLLIN;
				conn->sbuf = conn->sbuf2;
				conn->ssize = conn->ssize2;
				conn->soffset = 0;
				ret = rsend(conn->rsock, conn->sbuf,
					    conn->ssize, MSG_DONTWAIT);
				if (ret > 0) {
					conn->soffset += ret;
					if (conn->soffset == conn->ssize)
						return POLLIN;
					else
						return POLLOUT | POLLIN;
				}
			} else
				return POLLOUT | POLLIN;
		}
	} else
		ssa_log_err(SSA_LOG_CTRL,
			    "failed to allocate ssa_msg_hdr for response to op %u\n",
			    len, op);
	return events;
}

static struct ssa_db *ssa_downstream_db(struct ssa_conn *conn)
{
	/* Use SSA DB if available; otherwise use preloaded DB */
	if (conn->ssa_db)
		return conn->ssa_db;
#ifdef ACCESS_INTEGRATION
	return prdb;
#else
	if (conn->dbtype == SSA_CONN_SMDB_TYPE)
		return smdb;
	return NULL;
#endif
}

static short ssa_downstream_handle_query_defs(struct ssa_conn *conn,
					      struct ssa_msg_hdr *hdr,
					      short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (!ssadb) {
ssa_log(SSA_LOG_DEFAULT, "No ssa_db or prdb as yet\n");
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send_resp(conn,
						   SSA_MSG_DB_QUERY_DEF,
						   events);
		return revents;
	}

	if (conn->phase == SSA_DB_IDLE) {
		conn->phase = SSA_DB_DEFS;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_DEF,
					      &ssadb->db_def,
					      sizeof(ssadb->db_def),
					      events);
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_IDLE for SSA_MSG_DB_QUERY_DEF\n",
			     conn->rsock, conn->phase);

	return revents;
}

static short ssa_downstream_handle_query_tbl_def(struct ssa_conn *conn,
						 struct ssa_msg_hdr *hdr,
						 short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_DEFS) {
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_TBL_DEF,
					      &ssadb->db_table_def,
					      sizeof(ssadb->db_table_def),
					      events);
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_DEFS for SSA_MSG_DB_QUERY_TBL_DEF\n",
			     conn->rsock, conn->phase);

	return revents;
}

static short ssa_downstream_handle_query_tbl_defs(struct ssa_conn *conn,
						  struct ssa_msg_hdr *hdr,
						  short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_DEFS) {
		conn->phase = SSA_DB_TBL_DEFS;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_TBL_DEF_DATASET,
					      ssadb->p_def_tbl,
					      ntohll(ssadb->db_table_def.set_size),
					      events);
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_DEFS for SSA_MSG_DB_QUERY_TBL_DEF_DATASET\n",
			     conn->rsock, conn->phase);
	return revents;
}

static short ssa_downstream_handle_query_field_defs(struct ssa_conn *conn,
						    struct ssa_msg_hdr *hdr,
						    short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_TBL_DEFS) {
		conn->phase = SSA_DB_FIELD_DEFS;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
					      ssadb->p_db_field_tables,
					      ssadb->data_tbl_cnt * sizeof(*ssadb->p_db_field_tables),
					      events);
		conn->sindex = 0;
	} else if (conn->phase == SSA_DB_FIELD_DEFS) {
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		if (conn->sindex < ssadb->data_tbl_cnt) {
ssa_log(SSA_LOG_DEFAULT, "pp_field_tables index %d %p len %d\n", conn->sindex, ssadb->pp_field_tables[conn->sindex], ntohll(ssadb->p_db_field_tables[conn->sindex].set_size));
			revents = ssa_downstream_send(conn,
						      SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
						      ssadb->pp_field_tables[conn->sindex],
						      ntohll(ssadb->p_db_field_tables[conn->sindex].set_size),
						      events);

			conn->sindex++;
		} else {
			revents = ssa_downstream_send_resp(conn,
							   SSA_MSG_DB_QUERY_FIELD_DEF_DATASET,
							   events);
		}
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_TBL_DEFS for SSA_MSG_DB_QUERY_FIELD_DEF_DATASET\n",
			     conn->rsock, conn->phase);
	return revents;
}

static short ssa_downstream_handle_query_data(struct ssa_conn *conn,
					      struct ssa_msg_hdr *hdr,
					      short events)
{
	struct ssa_db *ssadb;
	short revents = events;

	ssadb = ssa_downstream_db(conn);
	if (conn->phase == SSA_DB_FIELD_DEFS) {
		conn->phase = SSA_DB_DATA;
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		revents = ssa_downstream_send(conn,
					      SSA_MSG_DB_QUERY_DATA_DATASET,
					      ssadb->p_db_tables,
					      ssadb->data_tbl_cnt * sizeof(*ssadb->p_db_tables),
					      events);
		conn->sindex = 0;
	} else if (conn->phase == SSA_DB_DATA) {
		conn->rid = ntohl(hdr->id);
		conn->roffset = 0;
		if (conn->sindex < ssadb->data_tbl_cnt) {
ssa_log(SSA_LOG_DEFAULT, "pp_tables index %d %p len %d\n", conn->sindex, ssadb->pp_tables[conn->sindex], ntohll(ssadb->p_db_tables[conn->sindex].set_size));
			revents = ssa_downstream_send(conn,
						      SSA_MSG_DB_QUERY_DATA_DATASET,
						      ssadb->pp_tables[conn->sindex],
						      ntohll(ssadb->p_db_tables[conn->sindex].set_size),
						      events);
			conn->sindex++;
		} else {
			revents = ssa_downstream_send_resp(conn,
							   SSA_MSG_DB_QUERY_DATA_DATASET,
							   events);
		}
	} else
		ssa_log_warn(SSA_LOG_CTRL,
			     "rsock %d phase %d not SSA_DB_DEFS for SSA_MSG_DB_QUERY_DATA_DATASET\n",
			     conn->rsock, conn->phase);
	return revents;
}

static short ssa_downstream_handle_op(struct ssa_conn *conn,
				      struct ssa_msg_hdr *hdr, short events)
{
	uint16_t op;
	short revents = events;

	op = ntohs(hdr->op);
	if (ntohs(hdr->flags) & SSA_MSG_FLAG_RESP)
		ssa_log(SSA_LOG_DEFAULT,
			"Ignoring SSA_MSG_FLAG_RESP set in op %u request in phase %d\n",
			op, conn->phase);
	switch (op) {
	case SSA_MSG_DB_QUERY_DEF:
		revents = ssa_downstream_handle_query_defs(conn, hdr, events);
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF:
		revents = ssa_downstream_handle_query_tbl_def(conn, hdr, events);
		break;
	case SSA_MSG_DB_QUERY_TBL_DEF_DATASET:
		revents = ssa_downstream_handle_query_tbl_defs(conn, hdr,
							       events);
		break;
	case SSA_MSG_DB_QUERY_FIELD_DEF_DATASET:
		revents = ssa_downstream_handle_query_field_defs(conn, hdr,
								 events);
		break;
	case SSA_MSG_DB_QUERY_DATA_DATASET:
		revents = ssa_downstream_handle_query_data(conn, hdr, events);
		break;
	case SSA_MSG_DB_PUBLISH_EPOCH_BUF:
		ssa_log_warn(SSA_LOG_CTRL,
			     "SSA_MSG_DB_PUBLISH_EPOCH_BUF not supported yet\n");
		break;
	default:
		ssa_log_warn(SSA_LOG_CTRL, "unknown op %u\n", op);
		break;
	}
	return revents;
}

static short ssa_downstream_rrecv(struct ssa_conn *conn, short events)
{
	struct ssa_msg_hdr *hdr;
	int ret;
	short revents = events;

	ret = rrecv(conn->rsock, conn->rbuf + conn->roffset,
		    conn->rsize - conn->roffset, MSG_DONTWAIT);
	if (ret > 0) {
		conn->roffset += ret;
		if (conn->roffset == conn->rsize) {
			hdr = conn->rbuf;
			if (validate_ssa_msg_hdr(hdr)) {
				revents = ssa_downstream_handle_op(conn, hdr, events);
			} else
				ssa_log_warn(SSA_LOG_CTRL,
					     "validate_ssa_msg_hdr failed: version %d class %d op %u id 0x%x\n",
					     hdr->version, hdr->class,
					     ntohs(hdr->op), ntohl(hdr->id));
		}
	}
	return revents;
}

static short ssa_downstream_handle_rsock_revents(struct ssa_conn *conn,
						 short events)
{
	short revents = events;

	if (events & POLLIN) {
		if (!conn->rbuf) {
			conn->rbuf = malloc(sizeof(struct ssa_msg_hdr));
			if (conn->rbuf) {
				conn->rsize = sizeof(struct ssa_msg_hdr);
				conn->roffset = 0;
			} else
				ssa_log_err(SSA_LOG_CTRL,
					    "failed to allocate ssa_msg_hdr for rrecv\n");
		}
		if (conn->rbuf) {
			revents = ssa_downstream_rrecv(conn, events);
		}
	}
	if (events & POLLOUT) {
		revents = ssa_rsend_continue(conn, events);
	}
	if (events & ~(POLLOUT | POLLIN)) {
		ssa_log(SSA_LOG_DEFAULT,
			"unexpected event 0x%x on data rsock %d\n",
			events & ~(POLLOUT | POLLIN), conn->rsock);
	}

	return revents;
}

static int ssa_find_pollfd_slot(struct pollfd *fds, int nfds)
{
	int i;

	for (i = FIRST_DATA_FD_SLOT; i < nfds; i++)
		if (fds[i].fd == -1)
			return i;
	return -1;
}

static void ssa_check_listen_events(struct ssa_svc *svc, struct pollfd *pfd,
				    struct pollfd **fds, int conn_dbtype)
{
	struct ssa_conn *conn_data;
	struct pollfd *pfd2;
	int fd, slot;

	conn_data = malloc(sizeof(*conn_data));
	if (conn_data) {
		ssa_init_ssa_conn(conn_data, SSA_CONN_TYPE_DOWNSTREAM,
				  conn_dbtype);
		fd = ssa_downstream_svc_server(svc, conn_data);
		if (fd >= 0) {
			if (!svc->fd_to_conn[fd]) {
				svc->fd_to_conn[fd] = conn_data;
				pfd2 = (struct  pollfd *)fds;
				slot = ssa_find_pollfd_slot(pfd2, FD_SETSIZE);
				if (slot >= 0) {
					pfd2 = (struct  pollfd *)(fds + slot);
					pfd2->fd = fd;
					pfd2->events = POLLIN;
					if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS)
						ssa_downstream_conn_done(svc, conn_data);
				} else
					ssa_log_warn(SSA_LOG_CTRL,
						     "no pollfd slot available\n");
			} else
				ssa_log_warn(SSA_LOG_CTRL,
					     "fd %d in fd_to_conn array already occupied\n",
					     fd);
		}
	} else
		ssa_log_err(SSA_LOG_DEFAULT,
			    "struct ssa_conn allocation failed\n");
}

static void *ssa_downstream_handler(void *context)
{
	struct ssa_svc *svc = context;
	struct ssa_ctrl_msg_buf msg;
	struct pollfd **fds;
	struct pollfd *pfd, *pfd2;
	int ret, i;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	write(svc->sock_downctrl[1], (char *) &msg, sizeof msg.hdr);

	fds = calloc(FD_SETSIZE, sizeof(**fds));
	if (!fds)
		goto out;
	pfd = (struct pollfd *)fds;
	pfd->fd = svc->sock_downctrl[1];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 1);
	pfd->fd = svc->sock_accessdown[0];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 2);
	pfd->fd = svc->sock_updown[1];
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 3);
	pfd->fd = -1;	/* placeholder for SMDB listen rsock */
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 4);
	pfd->fd = -1;	/* placeholder for PRDB listen rsock */
	pfd->events = POLLIN;
	pfd->revents = 0;
	pfd = (struct pollfd *)(fds + 5);
	pfd->fd = svc->sock_extractdown[0];
	pfd->events = POLLIN;
	pfd->revents = 0;
	for (i = FIRST_DATA_FD_SLOT; i < FD_SETSIZE; i++) {
		pfd = (struct pollfd *)(fds + i);
		pfd->fd = -1;	/* placeholder for downstream connections */
		pfd->events = 0;
		pfd->revents = 0;
	}

	for (;;) {
		ret = rpoll((struct pollfd *)fds, FD_SETSIZE, -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}
		pfd = (struct pollfd *)fds;
		if (pfd->revents) {
			pfd->revents = 0;
			read(svc->sock_downctrl[1], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_downctrl[1],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_LISTEN:
				if (svc->port->dev->ssa->node_type &
				    (SSA_NODE_CORE | SSA_NODE_DISTRIBUTION)) {
					pfd2 = (struct pollfd *)(fds + 3);
					pfd2->fd = ssa_downstream_listen(svc, &svc->conn_listen_smdb, smdb_port);
				}

				if (svc->port->dev->ssa->node_type &
				    SSA_NODE_ACCESS) {
					pfd2 = (struct pollfd *)(fds + 4);
					pfd2->fd = ssa_downstream_listen(svc, &svc->conn_listen_prdb, prdb_port);
				}
				break;
			case SSA_CTRL_EXIT:
				goto out;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from ctrl\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 1);
		if (pfd->revents) {
			pfd->revents = 0;
			read(svc->sock_accessdown[0], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_accessdown[0],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE:
ssa_sprint_addr(SSA_LOG_DEFAULT, log_data, sizeof log_data, SSA_ADDR_GID, msg.data.db_upd.remote_gid->raw, sizeof msg.data.db_upd.remote_gid->raw);
ssa_log(SSA_LOG_DEFAULT, "SSA DB update: rsock %d GID %s ssa_db %p\n", msg.data.db_upd.rsock, log_data, msg.data.db_upd.db);
				/* Now ready to rsend to downstream client upon request */
				if (svc->fd_to_conn[msg.data.db_upd.rsock])
					svc->fd_to_conn[msg.data.db_upd.rsock]->ssa_db = msg.data.db_upd.db;
				else
					ssa_log_warn(SSA_LOG_CTRL,
						     "DB update for rsock %d but no ssa_conn struct available\n");
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from access\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 2);
		if (pfd->revents) {
			pfd->revents = 0;
			read(svc->sock_updown[1], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_updown[1],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE:
ssa_log(SSA_LOG_DEFAULT, "SSA DB update (SMDB): ssa_db %p\n", msg.data.db_upd.db);
				smdb = msg.data.db_upd.db;
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from upstream\n",
					     msg.hdr.type);
				break;
			}
		}

		pfd = (struct pollfd *)(fds + 3);
		if (pfd->revents) {
			pfd->revents = 0;
			ssa_check_listen_events(svc, pfd, fds,
						SSA_CONN_SMDB_TYPE);
		}

		pfd = (struct pollfd *)(fds + 4);
		if (pfd->revents) {
			pfd->revents = 0;
			ssa_check_listen_events(svc, pfd, fds,
						SSA_CONN_PRDB_TYPE);
		}

		pfd = (struct pollfd *)(fds + 5);
		if (pfd->revents) {
			pfd->revents = 0;
			read(svc->sock_extractdown[0], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_extractdown[0],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif
			switch (msg.hdr.type) {
			case SSA_DB_UPDATE:
				ssa_log(SSA_LOG_DEFAULT, "SSA DB update (SMDB): "
					"ssa_db %p\n", msg.data.db_upd.db);
				smdb = msg.data.db_upd.db;
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from upstream\n",
					     msg.hdr.type);
				break;
			}
		}

		for (i = FIRST_DATA_FD_SLOT; i < FD_SETSIZE; i++) {
			pfd = (struct pollfd *)(fds + i);
			if (pfd->revents) {
				if (svc->fd_to_conn[pfd->fd]) {
					pfd->events = ssa_downstream_handle_rsock_revents(svc->fd_to_conn[pfd->fd], pfd->revents);
				} else
					ssa_log_warn(SSA_LOG_CTRL,
						     "event 0x%x but no data rsock for pollfd slot %d\n",
						     pfd->revents, i);
			}
			pfd->revents = 0;
		}

	}

out:
	return NULL;
}

static void ssa_access_send_db_update(struct ssa_svc *svc, struct ssa_db *db,
				      int rsock, int flags,
				      union ibv_gid *remote_gid)
{
	struct ssa_db_update_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_DB_UPDATE;
	msg.hdr.len = sizeof(msg);
	msg.db_upd.db = db;
	msg.db_upd.rsock = rsock;
	msg.db_upd.flags = flags;
	msg.db_upd.remote_gid = remote_gid;
	write(svc->sock_accessdown[1], (char *) &msg, sizeof(msg));
}

static void *ssa_access_handler(void *context)
{
	struct ssa_svc *svc = context;
	struct ssa_ctrl_msg_buf msg;
	struct pollfd fds[3];
	struct ssa_db *prdb = NULL;
	int ret;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	write(svc->sock_accessctrl[1], (char *) &msg, sizeof msg.hdr);

	fds[0].fd = svc->sock_accessctrl[1];
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	fds[1].fd = svc->sock_accessup[1];
	fds[1].events = POLLIN;
	fds[1].revents = 0;
	fds[2].fd = svc->sock_accessdown[1];
	fds[2].events = POLLIN;
	fds[2].revents = 0;

	if (!access_context.context) {
		ssa_log_err(SSA_LOG_CTRL, "access context is empty\n");
		goto out;
	}
#ifdef ACCESS_INTEGRATION
	if (!access_context.smdb) {
		ssa_log_err(SSA_LOG_CTRL, "smdb database is empty\n");
		goto out;
	}
#endif

	for (;;) {
		ret = poll(&fds[0], 3, -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}
		if (fds[0].revents) {
			fds[0].revents = 0;
			read(svc->sock_accessctrl[1], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_accessctrl[1],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_CTRL_EXIT:
				goto out;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from ctrl\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[1].revents) {
			fds[1].revents = 0;
			read(svc->sock_accessup[1], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_accessup[1],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_DB_UPDATE:
ssa_log(SSA_LOG_DEFAULT, "SSA DB update: ssa_db %p\n", msg.data.db_upd.db);
				access_context.smdb = msg.data.db_upd.db;
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from upstream\n",
					     msg.hdr.type);
				break;
			}
		}

		if (fds[2].revents) {
			fds[2].revents = 0;
			read(svc->sock_accessdown[1], (char *) &msg, sizeof msg.hdr);
			if (msg.hdr.len > sizeof msg.hdr) {
				read(svc->sock_accessdown[1],
				     (char *) &msg.hdr.data,
				     msg.hdr.len - sizeof msg.hdr);
			}
#if 0
			if (svc->process_msg && svc->process_msg(svc, &msg))
				continue;
#endif

			switch (msg.hdr.type) {
			case SSA_CONN_DONE:
				ssa_sprint_addr(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
						log_data, sizeof log_data,
						SSA_ADDR_GID,
						msg.data.conn->remote_gid.raw,
						sizeof msg.data.conn->remote_gid.raw);
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					"connection done on rsock %d from GID %s\n",
					msg.data.conn->rsock, log_data);
				/* First, calculate half world PathRecords for GID */
				/* ssa_calc_path_records(); */
				/* Now, tell downstream where this ssa_db struct is */
				/* Replace NULL with pointer to real struct ssa_db */
#ifdef ACCESS
				if (access_context.smdb) {
					/* This call pulls in access layer for all node types !!! */
					prdb = ssa_pr_compute_half_world(access_context.smdb,
									 access_context.context,
									 msg.data.conn->remote_gid.global.interface_id);
#endif
					if (!prdb) {
						ssa_log_err(SSA_LOG_CTRL,
							    "prdb creation for GID %s\n",
							    log_data);
						continue;
					}
					ssa_access_send_db_update(svc, prdb,
								  msg.data.conn->rsock, 0,
								  &msg.data.conn->remote_gid);
					/*
					 * TODO: destroy prdb database
					 * ssa_db_destroy(prdb);
					 */
#ifdef ACCESS
				} else
					ssa_log_err(SSA_LOG_CTRL,
						    "smdb database is empty\n");
#endif
				break;
			default:
				ssa_log_warn(SSA_LOG_CTRL,
					     "ignoring unexpected message type %d from downstream\n",
					     msg.hdr.type);
				break;
			}
		}
	}

out:
	return NULL;
}

static void ssa_ctrl_port_send(struct ssa_port *port, struct ssa_ctrl_msg *msg)
{
	int i;
	for (i = 0; i < port->svc_cnt; i++)
		write(port->svc[i]->sock_upctrl[0], msg, msg->len);
}

/*
static void ssa_ctrl_dev_send(struct ssa_device *dev, struct ssa_ctrl_msg *msg)
{
	int i;
	for (i = 1; i <= dev->port_cnt; i++)
		ssa_ctrl_port_send(ssa_dev_port(dev, i), msg);
}
*/

static void ssa_ctrl_send_event(struct ssa_port *port, enum ibv_event_type event)
{
	struct ssa_ctrl_dev_event_msg msg;

	msg.hdr.len = sizeof msg;
	msg.hdr.type = SSA_CTRL_DEV_EVENT;
	msg.event = event;
	ssa_ctrl_port_send(port, &msg.hdr);
}

static void ssa_ctrl_update_port(struct ssa_port *port)
{
	struct ibv_port_attr attr;

	ibv_query_port(port->dev->verbs, port->port_num, &attr);
	if (attr.state == IBV_PORT_ACTIVE) {
		port->sm_lid = attr.sm_lid;
		port->sm_sl = attr.sm_sl;
		ibv_query_gid(port->dev->verbs, port->port_num, 0, &port->gid);
	}
	port->state = attr.state;
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s state %s SM LID %d\n",
		port->name, ibv_port_state_str(port->state), port->sm_lid);
}

static void ssa_ctrl_device(struct ssa_device *dev)
{
	struct ibv_async_event event;
	int ret;

	ssa_log(SSA_LOG_CTRL, "%s\n", dev->name);
	ret = ibv_get_async_event(dev->verbs, &event);
	if (ret)
		return;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
		"async event %s\n", ibv_event_type_str(event.event_type));
	switch (event.event_type) {
	case IBV_EVENT_PORT_ACTIVE:
	case IBV_EVENT_CLIENT_REREGISTER:
	case IBV_EVENT_PORT_ERR:
		ssa_ctrl_update_port(ssa_dev_port(dev, event.element.port_num));
		ssa_ctrl_send_event(ssa_dev_port(dev, event.element.port_num),
				    event.event_type);
		break;
	default:
		break;
	}

	ibv_ack_async_event(&event);
}

static void ssa_ctrl_send_listen(struct ssa_svc *svc)
{
	struct ssa_listen_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_LISTEN;
	msg.hdr.len = sizeof(msg);
	msg.svc = svc;
	write(svc->sock_downctrl[0], (char *) &msg, sizeof(msg));
}

static void ssa_ctrl_port(struct ssa_port *port)
{
	struct ssa_svc *svc;
	struct ssa_ctrl_umad_msg msg;
	struct ssa_member_record *member_rec;
	struct ssa_info_record *info_rec;
	int len, ret, parent = 0;

	ssa_log(SSA_LOG_CTRL, "%s receiving MAD\n", port->name);
	len = sizeof msg.umad;
	ret = umad_recv(port->mad_portid, (void *) &msg.umad, &len, 0);
	if (ret < 0) {
		ssa_log_warn(SSA_LOG_CTRL, "receive MAD failure\n");
		return;
	}

	if ((msg.umad.packet.mad_hdr.method & UMAD_METHOD_RESP_MASK) ||
	     msg.umad.umad.status) {
		svc = ssa_svc_from_tid(port, msg.umad.packet.mad_hdr.tid);
		if (msg.umad.packet.mad_hdr.mgmt_class == UMAD_CLASS_SUBN_ADM)
			msg.hdr.type = SSA_SA_MAD;
		else
			msg.hdr.type = SSA_CTRL_MAD;
	} else {
		switch (ntohs(msg.umad.packet.mad_hdr.attr_id)) {
		case SSA_ATTR_INFO_REC:
			parent = 1;
			info_rec = (struct ssa_info_record *) msg.umad.packet.data;
			svc = ssa_find_svc(port, ntohll(info_rec->database_id));
			break;
		case SSA_ATTR_MEMBER_REC:
			member_rec = (struct ssa_member_record *) msg.umad.packet.data;
			svc = ssa_find_svc(port, ntohll(member_rec->database_id));
			break;
		default:
			svc = NULL;
			break;
		}
		msg.hdr.type = SSA_CTRL_MAD;
	}

	if (!svc) {
		ssa_log_err(SSA_LOG_CTRL, "no matching service for received MAD\n");
		return;
	}

	msg.hdr.len = sizeof msg;
	/* set qkey for possible response */
	msg.umad.umad.addr.qkey = htonl(UMAD_QKEY);
	write(svc->sock_upctrl[0], (void *) &msg, msg.hdr.len);

	if (parent)
		ssa_ctrl_send_listen(svc);
}

static void ssa_upstream_conn_done(struct ssa_svc *svc, struct ssa_conn *conn)
{
	struct ssa_conn_done_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_CONN_DONE;
	msg.hdr.len = sizeof(msg);
	msg.conn = conn;
	write(svc->sock_upctrl[0], (char *) &msg, sizeof msg);
}

static void ssa_upstream_svc_client(struct ssa_svc *svc, int errnum)
{
	int ret, err;
	socklen_t len;

	if (errnum == EINPROGRESS)
		return;

	if (svc->conn_dataup.state!= SSA_CONN_CONNECTING) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"Unexpected consumer event in state %d\n",
			svc->conn_dataup.state);
		return;
	}

	len = sizeof err;
	ret = rgetsockopt(svc->conn_dataup.rsock, SOL_SOCKET, SO_ERROR,
			  &err, &len);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rgetsockopt fd %d ERROR %d (%s)\n",
			svc->conn_dataup.rsock, errno, strerror(errno));
		return;
	}
	if (err) {
		errno = err;
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"async rconnect fd %d ERROR %d (%s)\n",
			svc->conn_dataup.rsock, errno, strerror(errno));
		return;
	}

	memcpy(&svc->conn_dataup.remote_gid, &svc->primary_parent.path.dgid,
	       sizeof(union ibv_gid));
	svc->conn_dataup.state = SSA_CONN_CONNECTED;
	svc->state = SSA_STATE_CONNECTED;

	ssa_upstream_conn_done(svc, &svc->conn_dataup);
}

static int ssa_downstream_svc_server(struct ssa_svc *svc, struct ssa_conn *conn)
{
	struct ssa_conn *conn_listen;
	int fd, val, ret;
	struct sockaddr_ib peer_addr;
	socklen_t peer_len;

	if (conn->dbtype == SSA_CONN_SMDB_TYPE)
		conn_listen = &svc->conn_listen_smdb;
	else
		conn_listen = &svc->conn_listen_prdb;
	fd = raccept(conn_listen->rsock, NULL, 0);
	if (fd < 0) {
		if ((errno == EAGAIN || errno == EWOULDBLOCK))
			return -1;	/* ignore these errors */
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"raccept fd %d ERROR %d (%s)\n",
			conn_listen->rsock, errno, strerror(errno));
		return -1;
	}

	ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
		"new connection accepted on fd %d dbtype %d\n",
		fd, conn->dbtype);

	peer_len = sizeof(peer_addr);
	if (!rgetpeername(fd, (struct sockaddr *) &peer_addr, &peer_len)) {
		if (peer_addr.sib_family == AF_IB) {
			ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
					log_data, sizeof log_data, SSA_ADDR_GID,
					(uint8_t *) &peer_addr.sib_addr,
					peer_len);
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"peer GID %s\n", log_data);
		} else {
			ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
				"rgetpeername fd %d family %d not AF_IB\n",
				fd, peer_addr.sib_family);
			rclose(fd);
			return -1;
		}
	} else {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rgetpeername fd %d ERROR %d (%s)\n",
			fd, errno, strerror(errno));
		rclose(fd);
		return -1;
	}

	val = 1;
	ret = rsetsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt TCP_NODELAY ERROR %d (%s)\n",
			errno, strerror(errno));
		rclose(fd);
		return -1;
	}
	ret = rfcntl(fd, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rfcntl ERROR %d (%s)\n",
			errno, strerror(errno));
		rclose(fd);
		return -1;
	}

	conn->rsock = fd;

	memcpy(&conn->remote_gid, &peer_addr.sib_addr, sizeof(union ibv_gid));
	conn->state = SSA_CONN_CONNECTED;
	svc->state = SSA_STATE_CONNECTED;

	return fd;
}

static int ssa_upstream_initiate_conn(struct ssa_svc *svc, short dport)
{
	struct sockaddr_ib dst_addr;
	int ret, val;

	svc->conn_dataup.rsock = rsocket(AF_IB, SOCK_STREAM, 0);
	if (svc->conn_dataup.rsock < 0) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsocket ERROR %d (%s)\n",
			errno, strerror(errno));
		return -1;
	}

	val = 1;
	ret = rsetsockopt(svc->conn_dataup.rsock, SOL_SOCKET, SO_REUSEADDR,
			  &val, sizeof val);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt SO_REUSEADDR ERROR %d (%s)\n",
			errno, strerror(errno));
		goto close;
	}

	ret = rsetsockopt(svc->conn_dataup.rsock, IPPROTO_TCP, TCP_NODELAY,
			  (void *) &val, sizeof(val));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt TCP_NODELAY ERROR %d (%s)\n",
			errno, strerror(errno));
		goto close;
	}
	ret = rfcntl(svc->conn_dataup.rsock, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rfcntl ERROR %d (%s)\n",
			errno, strerror(errno));
		goto close;
	}

	ret = rsetsockopt(svc->conn_dataup.rsock, SOL_RDMA, RDMA_ROUTE,
			  &svc->primary_parent, sizeof(svc->primary_parent));
	if (ret) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rsetsockopt RDMA_ROUTE ERROR %d (%s)\n",
			errno, strerror(errno));
		goto close;
	}

	dst_addr.sib_family = AF_IB;
	dst_addr.sib_pkey = 0xFFFF;
	dst_addr.sib_flowinfo = 0;
	dst_addr.sib_sid = htonll(((uint64_t) RDMA_PS_TCP << 16) + dport);
	dst_addr.sib_sid_mask = htonll(RDMA_IB_IP_PS_MASK);
	dst_addr.sib_scope_id = 0;
	memcpy(&dst_addr.sib_addr, &svc->primary_parent.path.dgid,
	       sizeof(union ibv_gid));
	ssa_sprint_addr(SSA_LOG_DEFAULT | SSA_LOG_CTRL, log_data, sizeof log_data,
			SSA_ADDR_GID, (uint8_t *) &dst_addr.sib_addr,
			sizeof dst_addr.sib_addr);
	ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL, "dest GID %s\n", log_data);

	ret = rconnect(svc->conn_dataup.rsock,
		       (const struct sockaddr *) &dst_addr, sizeof(dst_addr));
	if (ret && (errno != EINPROGRESS)) {
		ssa_log(SSA_LOG_DEFAULT | SSA_LOG_CTRL,
			"rconnect ERROR %d (%s)\n",
			errno, strerror(errno));
		goto close;
	}

	svc->conn_dataup.state = SSA_CONN_CONNECTING;
	svc->state = SSA_STATE_CONNECTING;

	if (ret == 0)
		ssa_upstream_svc_client(svc, 0);

	return svc->conn_dataup.rsock;

close:
	rclose(svc->conn_dataup.rsock);
	svc->conn_dataup.rsock = -1;
	return -1;
}

static int ssa_ctrl_init_fds(struct ssa_class *ssa)
{
	struct ssa_device *dev;
	struct ssa_port *port;
	int d, p, i = 0;

	ssa->nfds = 1;			/* ssa socketpair */
	ssa->nfds += ssa->dev_cnt;	/* async device events */
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		ssa->nfds += dev->port_cnt;	/* mads */
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			ssa->nsfds += port->svc_cnt;	/* service listen */
		}
	}
	ssa->nsfds++;

	ssa->fds = calloc(ssa->nfds + ssa->nsfds,
			  sizeof(*ssa->fds) + sizeof(*ssa->fds_obj));
	if (!ssa->fds)
		return seterr(ENOMEM);

	ssa->fds_obj = (struct ssa_obj *) (&ssa->fds[ssa->nfds + ssa->nsfds]);
	ssa->fds[i].fd = ssa->sock[1];
	ssa->fds[i].events = POLLIN;
	ssa->fds_obj[i++].type = SSA_OBJ_CLASS;
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		ssa->fds[i].fd = dev->verbs->async_fd;
		ssa->fds[i].events = POLLIN;
		ssa->fds_obj[i].type = SSA_OBJ_DEVICE;
		ssa->fds_obj[i++].dev = dev;

		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			ssa->fds[i].fd = umad_get_fd(port->mad_portid);
			ssa->fds[i].events = POLLIN;
			ssa->fds_obj[i].type = SSA_OBJ_PORT;
			ssa->fds_obj[i++].port = port;
		}
	}
	return 0;
}

static void ssa_ctrl_activate_ports(struct ssa_class *ssa)
{
	struct ssa_device *dev;
	struct ssa_port *port;
	int d, p;

	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++) {
			port = ssa_dev_port(dev, p);
			ssa_ctrl_update_port(port);
			if (port->state == IBV_PORT_ACTIVE) {
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", port->name);
				ssa_ctrl_send_event(port, IBV_EVENT_PORT_ACTIVE);
			}
		}
	}
}

int ssa_ctrl_run(struct ssa_class *ssa)
{
	struct ssa_ctrl_msg_buf msg;
	int i, ret;
	struct ssa_conn_req_msg *conn_req;

	ssa_log_func(SSA_LOG_CTRL);
	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, ssa->sock);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating socketpair\n");
		return ret;
	}

	ret = ssa_ctrl_init_fds(ssa);
	if (ret)
		goto err;

	ssa_ctrl_activate_ports(ssa);

	for (;;) {
		ret = rpoll(ssa->fds, ssa->nfds, -1);
		if (ret < 0) {
			ssa_log_err(SSA_LOG_CTRL, "polling fds %d (%s)\n",
				    errno, strerror(errno));
			continue;
		}

		for (i = 0; i < ssa->nfds; i++) {
			if (!ssa->fds[i].revents)
				continue;

			ssa->fds[i].revents = 0;
			switch (ssa->fds_obj[i].type) {
			case SSA_OBJ_CLASS:
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					"class event on fd %d\n", ssa->fds[i]);

				read(ssa->sock[1], (char *) &msg, sizeof msg.hdr);
				if (msg.hdr.len > sizeof msg.hdr)
					read(ssa->sock[1],
					     (char *) &msg.hdr.data,
					     msg.hdr.len - sizeof msg.hdr);
				switch (msg.hdr.type) {
				case SSA_CONN_REQ:
					conn_req = (struct ssa_conn_req_msg *) &msg;
					write(conn_req->svc->sock_upctrl[0],
					      (char *) &msg,
					      sizeof(struct ssa_conn_req_msg));
					break;
				case SSA_CTRL_EXIT:
					goto out;
				default:
					ssa_log_warn(SSA_LOG_CTRL,
						     "ignoring unexpected message type %d\n",
						     msg.hdr.type);
					break;
				}
				break;
			case SSA_OBJ_DEVICE:
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					"device event on fd %d\n", ssa->fds[i].fd);
				ssa_ctrl_device(ssa->fds_obj[i].dev);
				break;
			case SSA_OBJ_PORT:
				ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
					"port event on fd %d\n", ssa->fds[i].fd);
				ssa_ctrl_port(ssa->fds_obj[i].port);
				break;
			}
		}
	}
out:
	msg.hdr.len = sizeof msg.hdr;
	msg.hdr.type = SSA_CTRL_ACK;
	write(ssa->sock[1], (char *) &msg, sizeof msg.hdr);
	free(ssa->fds);
	return 0;

err:
	close(ssa->sock[0]);
	close(ssa->sock[1]);
	return ret;
}

void ssa_ctrl_conn(struct ssa_class *ssa, struct ssa_svc *svc)
{
	struct ssa_conn_req_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.hdr.type = SSA_CONN_REQ;
	msg.hdr.len = sizeof msg;
	msg.svc = svc;
	write(ssa->sock[0], (char *) &msg, sizeof msg);
}

void ssa_ctrl_stop(struct ssa_class *ssa)
{
	struct ssa_ctrl_msg msg;

	ssa_log_func(SSA_LOG_CTRL);
	msg.len = sizeof msg;
	msg.type = SSA_CTRL_EXIT;
	write(ssa->sock[0], (char *) &msg, sizeof msg);
	read(ssa->sock[0], (char *) &msg, sizeof msg);

	close(ssa->sock[0]);
	close(ssa->sock[1]);
}

struct ssa_svc *ssa_start_svc(struct ssa_port *port, uint64_t database_id,
			      size_t svc_size,
			      int (*process_msg)(struct ssa_svc *svc,
					         struct ssa_ctrl_msg_buf *msg))
{
	struct ssa_svc *svc, **list;
	struct ssa_ctrl_msg msg;
	int ret;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s:%llu\n",
		port->name, database_id);
	list = realloc(port->svc, (port->svc_cnt + 1) * sizeof(svc));
	if (!list)
		return NULL;

	port->svc = list;
	svc = calloc(1, svc_size);
	if (!svc)
		return NULL;

	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_upctrl);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating upstream/ctrl socketpair\n");
		goto err1;
	}

	if (port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_downctrl);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating downstream/ctrl socketpair\n");
			goto err2;
		}
	} else {
		svc->sock_downctrl[0] = -1;
		svc->sock_downctrl[1] = -1;
	}

	if (port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_accessctrl);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating access/ctrl socketpair\n");
			goto err3;
		}
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_accessup);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating access/upstream socketpair\n");
			goto err4;
		}
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_accessdown);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating access/downstream socketpair\n");
			goto err5;
		}
	} else {
		svc->sock_accessctrl[0] = -1;
		svc->sock_accessctrl[1] = -1;
		svc->sock_accessup[0] = -1;
		svc->sock_accessup[1] = -1;
		svc->sock_accessdown[0] = -1;
		svc->sock_accessdown[1] = -1;
	}

	if (port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_updown);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL,
				    "creating upstream/downstream socketpair\n");
			goto err6;
		}
	} else {
		svc->sock_updown[0] = -1;
		svc->sock_updown[1] = -1;
	}

	if (port->dev->ssa->node_type & SSA_NODE_CORE) {
		ret = socketpair(AF_UNIX, SOCK_STREAM, 0, svc->sock_extractdown);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL, "creating extract/downstream socketpair\n");
			goto err7;
		}
	} else {
		svc->sock_extractdown[0] = -1;
		svc->sock_extractdown[1] = -1;
	}

	svc->index = port->svc_cnt;
	svc->port = port;
	snprintf(svc->name, sizeof svc->name, "%s:%llu", port->name,
		 (unsigned long long) database_id);
	svc->database_id = database_id;
	svc->conn_listen_smdb.rsock = -1;
	svc->conn_listen_smdb.type = SSA_CONN_TYPE_UPSTREAM;
	svc->conn_listen_smdb.dbtype = SSA_CONN_SMDB_TYPE;
	svc->conn_listen_smdb.state = SSA_CONN_IDLE;
	svc->conn_listen_smdb.phase = SSA_DB_IDLE;
	svc->conn_listen_prdb.rsock = -1;
	svc->conn_listen_prdb.type = SSA_CONN_TYPE_UPSTREAM;
	svc->conn_listen_prdb.dbtype = SSA_CONN_PRDB_TYPE;
	svc->conn_listen_prdb.state = SSA_CONN_IDLE;
	svc->conn_listen_prdb.phase = SSA_DB_IDLE;
	ssa_init_ssa_conn(&svc->conn_dataup, SSA_CONN_TYPE_UPSTREAM,
			  SSA_CONN_NODB_TYPE);
	svc->state = SSA_STATE_IDLE;
	svc->process_msg = process_msg;
	//pthread_mutex_init(&svc->lock, NULL);

	ret = pthread_create(&svc->upstream, NULL, ssa_upstream_handler, svc);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "creating upstream thread\n");
		errno = ret;
		goto err8;
	}

	ret = read(svc->sock_upctrl[0], (char *) &msg, sizeof msg);
	if ((ret != sizeof msg) || (msg.type != SSA_CTRL_ACK)) {
		ssa_log_err(SSA_LOG_CTRL, "with upstream thread\n");
		goto err9;

	}

	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		ret = pthread_create(&svc->downstream, NULL,
				     ssa_downstream_handler, svc);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL, "creating downstream thread\n");
			errno = ret;
			goto err9;
		}

		ret = read(svc->sock_downctrl[0], (char *) &msg, sizeof msg);
		if ((ret != sizeof msg) || (msg.type != SSA_CTRL_ACK)) {
			ssa_log_err(SSA_LOG_CTRL, "with downstream thread\n");
			goto err10;
		}
	}

	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		ret = pthread_create(&svc->access, NULL, ssa_access_handler, svc);
		if (ret) {
			ssa_log_err(SSA_LOG_CTRL, "creating access thread\n");
			errno = ret;
			goto err10;
		}

		ret = read(svc->sock_accessctrl[0], (char *) &msg, sizeof msg);
		if ((ret != sizeof msg) || (msg.type != SSA_CTRL_ACK)) {
			ssa_log_err(SSA_LOG_CTRL, "with access thread\n");
			goto err11;
		}
	}

	port->svc[port->svc_cnt++] = svc;
	return svc;

err11:
	pthread_join(svc->access, NULL);
err10:
	pthread_join(svc->downstream, NULL);
err9:
	pthread_join(svc->upstream, NULL);
err8:
	if (svc->port->dev->ssa->node_type & SSA_NODE_CORE) {
		close(svc->sock_extractdown[0]);
		close(svc->sock_extractdown[1]);
	}
err7:
	if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		close(svc->sock_updown[0]);
		close(svc->sock_updown[1]);
	}
err6:
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		close(svc->sock_accessdown[0]);
		close(svc->sock_accessdown[1]);
	}
err5:
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		close(svc->sock_accessup[0]);
		close(svc->sock_accessup[1]);
	}
err4:
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		close(svc->sock_accessctrl[0]);
		close(svc->sock_accessctrl[1]);
	}
err3:
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		close(svc->sock_downctrl[0]);
		close(svc->sock_downctrl[1]);
	}
err2:
	close(svc->sock_upctrl[0]);
	close(svc->sock_upctrl[1]);
err1:
	free(svc);
	return NULL;
}

static void ssa_open_port(struct ssa_port *port, struct ssa_device *dev,
			  uint8_t port_num)
{
	long methods[16 / sizeof(long)];
	int ret;

	port->dev = dev;
	port->port_num = port_num;
	snprintf(port->name, sizeof port->name, "%s:%d", dev->name, port_num);
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", port->name);
	//pthread_mutex_init(&port->lock, NULL);

	port->mad_portid = umad_open_port(dev->name, port->port_num);
	if (port->mad_portid < 0) {
		ssa_log_err(SSA_LOG_CTRL, "unable to open MAD port %s\n",
			    port->name);
		return;
	}

	ret = fcntl(umad_get_fd(port->mad_portid), F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log_warn(SSA_LOG_CTRL, "MAD fd is blocking\n");
	}

	memset(methods, 0xFF, sizeof methods);
	port->mad_agentid = umad_register(port->mad_portid,
		SSA_CLASS, SSA_CLASS_VERSION, 0, methods);
	if (port->mad_agentid < 0) {
		ssa_log_err(SSA_LOG_CTRL, "unable to register SSA class on port %s\n",
			    port->name);
		goto err;
	}

	/* Only registering for solicited SA MADs */
	port->sa_agentid = umad_register(port->mad_portid,
		UMAD_CLASS_SUBN_ADM, UMAD_SA_CLASS_VERSION, 0, NULL);
	if (port->sa_agentid < 0) {
		ssa_log_err(SSA_LOG_CTRL, "unable to register SA class on port %s\n",
			    port->name);
		goto err2;
	}

	return;
err2:
	umad_unregister(port->mad_portid, port->mad_agentid);
err:
	umad_close_port(port->mad_portid);
}

static void ssa_open_dev(struct ssa_device *dev, struct ssa_class *ssa,
			 struct ibv_device *ibdev)
{
	struct ibv_device_attr attr;
	int i, ret;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", ibdev->name);
	dev->verbs = ibv_open_device(ibdev);
	if (dev->verbs == NULL) {
		ssa_log_err(SSA_LOG_CTRL, "opening device %s\n", ibdev->name);
		return;
	}

	ret = ibv_query_device(dev->verbs, &attr);
	if (ret) {
		ssa_log_err(SSA_LOG_CTRL, "ibv_query_device (%s) %d\n",
			    ibdev->name, ret);
		goto err1;
	}

	ret = fcntl(dev->verbs->async_fd, F_SETFL, O_NONBLOCK);
	if (ret) {
		ssa_log_warn(SSA_LOG_CTRL, "event fd is blocking\n");
	}

	dev->port = (struct ssa_port *) calloc(attr.phys_port_cnt, ssa->port_size);
	if (!dev)
		goto err1;

	dev->ssa = ssa;
	dev->guid = ibv_get_device_guid(ibdev);
	snprintf(dev->name, sizeof dev->name, ibdev->name);
	dev->port_cnt = attr.phys_port_cnt;
	dev->port_size = ssa->port_size;

	for (i = 1; i <= dev->port_cnt; i++)
		ssa_open_port(ssa_dev_port(dev, i), dev, i);

#ifdef CORE_INTEGRATION
	if (dev->ssa->node_type & SSA_NODE_CORE) {
		/* if configured, invoke SMDB preloading */

		smdb = ssa_db_load(SMDB_PRELOAD_PATH, SSA_DB_HELPER_DEBUG);
		if (!smdb) {
			ssa_log_err(SSA_LOG_CTRL,
				    "unable to preload smdb database. path:\"%s\"\n",
				    SMDB_PRELOAD_PATH);
		} else {
			ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
				"smdb is preloaded from \"%s\"\n",
				SMDB_PRELOAD_PATH);
		}
	}
#endif

	if (dev->ssa->node_type & SSA_NODE_ACCESS) {
#ifdef ACCESS_INTEGRATION
		/* if configured, invoke PR and/or SSA DB preloading */

		prdb = ssa_db_load(PRDB_PRELOAD_PATH, SSA_DB_HELPER_DEBUG);
		if (!prdb) {
			ssa_log_err(SSA_LOG_CTRL,
				    "unable to preload prdb database. path:\"%s\"\n",
				    PRDB_PRELOAD_PATH);
		} else {
			ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
				"prdb is preloaded from \"%s\"\n",
				PRDB_PRELOAD_PATH);
		}
#endif

#ifdef ACCESS
		/*
		 * TODO:
		 * 1. Pass required log verbosity. Now access layer has:
		 *  SSA_PR_NO_LOG = 0,
		 *  SSA_PR_EEROR_LEVEL = 1,
		 *  SSA_PR_INFO_LEVEL = 2,
		 *  SSA_PR_DEBUG_LEVEL = 3
		 * 2. Change errno
		 */
		if (!access_context.context)
			access_context.context = ssa_pr_create_context(flog, 0);
		if (!access_context.context) {
			ssa_log_err(SSA_LOG_CTRL,
				    "unable to create access layer context\n");
			goto ctx_create_err;
		}
#endif

#ifdef ACCESS_INTEGRATION
		if (!access_context.smdb)
			access_context.smdb = ssa_db_load(SMDB_PRELOAD_PATH,
							  SSA_DB_HELPER_DEBUG);
		if (!access_context.smdb) {
			ssa_log_err(SSA_LOG_CTRL,
				    "unable to preload smdb database. path:\"%s\"\n",
				    SMDB_PRELOAD_PATH);
			goto ctx_create_err;
		}
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL,
			"access context is created, smdb is preloaded from \"%s\"\n",
			SMDB_PRELOAD_PATH);
#endif
	}

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s opened\n", dev->name);
	return;

err1:
	ibv_close_device(dev->verbs);
	dev->verbs = NULL;

#ifdef ACCESS
ctx_create_err:
	if (access_context.context) {
		ssa_pr_destroy_context(access_context.context);
		access_context.context = NULL;
	}
#endif
#ifdef ACCESS_INTEGRATION
	if (access_context.smdb) {
		ssa_db_destroy(access_context.smdb);
		access_context.smdb = NULL;
	}
#endif
	seterr(ENOMEM);
}

int ssa_open_devices(struct ssa_class *ssa)
{
	struct ibv_device **ibdev;
	int i, ret = 0;

	/*
	 * TODO:
	 * Destroy old context if it exists: ssa_pr_destroy_context
	 */
	access_context.smdb = NULL;
	access_context.context = NULL;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);
	ibdev = ibv_get_device_list(&ssa->dev_cnt);
	if (!ibdev) {
		ssa_log_err(SSA_LOG_CTRL, "unable to get device list\n");
		return -1;
	}

	ssa->dev = (struct ssa_device *) calloc(ssa->dev_cnt, ssa->dev_size);
	if (!ssa->dev) {
		ssa_log_err(SSA_LOG_CTRL, "allocating devices\n");
		ret = seterr(ENOMEM);
		goto free;
	}

	for (i = 0; i < ssa->dev_cnt; i++)
		ssa_open_dev(ssa_dev(ssa, i), ssa, ibdev[i]);

free:
	ibv_free_device_list(ibdev);
	return ret;
}

static void ssa_stop_svc(struct ssa_svc *svc)
{
	struct ssa_ctrl_msg msg;
	int i;

	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", svc->name);
	msg.len = sizeof msg;
	msg.type = SSA_CTRL_EXIT;
	write(svc->sock_upctrl[0], (char *) &msg, sizeof msg);
	pthread_join(svc->upstream, NULL);
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		write(svc->sock_accessctrl[0], (char *) &msg, sizeof msg);
		pthread_join(svc->access, NULL);
	}
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		write(svc->sock_downctrl[0], (char *) &msg, sizeof msg);
		pthread_join(svc->downstream, NULL);
	}

	svc->port->svc[svc->index] = NULL;
	if (svc->conn_listen_smdb.rsock >= 0)
		ssa_close_ssa_conn(&svc->conn_listen_smdb);
	if (svc->conn_listen_prdb.rsock >= 0)
		ssa_close_ssa_conn(&svc->conn_listen_prdb);
	if (svc->port->dev->ssa->node_type & SSA_NODE_CORE) {
		close(svc->sock_extractdown[0]);
		close(svc->sock_extractdown[1]);
	}
	if (svc->port->dev->ssa->node_type & SSA_NODE_DISTRIBUTION) {
		close(svc->sock_updown[0]);
		close(svc->sock_updown[1]);
	}
	if (svc->port->dev->ssa->node_type & SSA_NODE_ACCESS) {
		close(svc->sock_accessdown[0]);
		close(svc->sock_accessdown[1]);
		close(svc->sock_accessup[0]);
		close(svc->sock_accessup[1]);
		close(svc->sock_accessctrl[0]);
		close(svc->sock_accessctrl[1]);
	}
	if (svc->conn_dataup.rsock >= 0)
		ssa_close_ssa_conn(&svc->conn_dataup);
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		for (i = 0; i < FD_SETSIZE; i++) {
			if (svc->fd_to_conn[i] &&
			    svc->fd_to_conn[i]->rsock >= 0) {
				ssa_close_ssa_conn(svc->fd_to_conn[i]);
				svc->fd_to_conn[i] = NULL;
			}
		}
	}
	if (svc->port->dev->ssa->node_type != SSA_NODE_CONSUMER) {
		close(svc->sock_downctrl[0]);
		close(svc->sock_downctrl[1]);
	}
	close(svc->sock_upctrl[0]);
	close(svc->sock_upctrl[1]);
	free(svc);
}

static void ssa_close_port(struct ssa_port *port)
{
	ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s\n", port->name);
	while (port->svc_cnt)
		ssa_stop_svc(port->svc[--port->svc_cnt]);
	if (port->svc)
		free(port->svc);

	if (port->sa_agentid >= 0)
		umad_unregister(port->mad_portid, port->sa_agentid);
	if (port->mad_agentid >= 0)
		umad_unregister(port->mad_portid, port->mad_agentid);
	if (port->mad_portid >= 0)
		umad_close_port(port->mad_portid);
}

void ssa_close_devices(struct ssa_class *ssa)
{
	struct ssa_device *dev;
	int d, p;

	ssa_log_func(SSA_LOG_VERBOSE | SSA_LOG_CTRL);
	for (d = 0; d < ssa->dev_cnt; d++) {
		dev = ssa_dev(ssa, d);
		for (p = 1; p <= dev->port_cnt; p++)
			ssa_close_port(ssa_dev_port(dev, p));

		ibv_close_device(dev->verbs);
		ssa_log(SSA_LOG_VERBOSE | SSA_LOG_CTRL, "%s closed\n", dev->name);
		free(dev->port);
	}
	free(ssa->dev);
	ssa->dev_cnt = 0;

#ifdef ACCESS
	if (access_context.context) {
		ssa_pr_destroy_context(access_context.context);
		access_context.context = NULL;
	}
#endif
#ifdef ACCESS_INTEGRATION
	if (access_context.smdb) {
		ssa_db_destroy(access_context.smdb);
		access_context.smdb = NULL;
	}
#endif
}

int ssa_open_lock_file(char *lock_file)
{
	int lock_fd;
	char pid[16];

	lock_fd = open(lock_file, O_RDWR | O_CREAT, 0640);
	if (lock_fd < 0)
		return lock_fd;

	if (lockf(lock_fd, F_TLOCK, 0)) {
		close(lock_fd);
		return -1;
	}

	snprintf(pid, sizeof pid, "%d\n", getpid());
	write(lock_fd, pid, strlen(pid));
	return 0;
}

void ssa_daemonize(void)
{
	pid_t pid, sid;

	pid = fork();
	if (pid)
		exit(pid < 0);

	sid = setsid();
	if (sid < 0)
		exit(1);

	if (chdir("/"))
		exit(1);

	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
}

int ssa_init(struct ssa_class *ssa, uint8_t node_type, size_t dev_size, size_t port_size)
{
	int ret;

	memset(ssa, 0, sizeof *ssa);
	ssa->node_type = node_type;
	ssa->dev_size = dev_size;
	ssa->port_size = port_size;
	ret = umad_init();
	if (ret)
		return ret;

	return 0;
}

void ssa_cleanup(struct ssa_class *ssa)
{
	umad_done();
}
