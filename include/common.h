/*
 * Copyright (c) 2009-2013 Intel Corporation. All rights reserved.
 *
 * This software is available to you under the OpenIB.org BSD license
 * below:
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
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
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
#include <infiniband/acm.h>
#include <infiniband/umad.h>
#include <infiniband/verbs.h>
#include <dlist.h>
#include <search.h>


void ssa_daemonize(void);
int ssa_open_lock_file(char *lock_file);

enum ssa_addr_type {
	SSA_ADDR_NAME,
	SSA_ADDR_IP,
	SSA_ADDR_IP6,
	SSA_ADDR_PATH,
	SSA_ADDR_GID,
	SSA_ADDR_LID
};

enum {
	SSA_LOG_DEFAULT		= 1 << 0,
	SSA_LOG_VERBOSE		= 1 << 1,
	SSA_LOG_CTRL		= 1 << 2,
	SSA_LOG_DB		= 1 << 3,
	SSA_LOG_COMM		= 1 << 4,
	SSA_LOG_ALL		= 0xFFFFFFFF,
};

extern __thread char log_data[128];

void ssa_set_log_level(int level);
int  ssa_open_log(char *log_file);
void ssa_close_log(void);
void ssa_write_log(int level, const char *format, ...);
#define ssa_log(level, format, ...) \
	ssa_write_log(level, "%s: "format, __func__, ## __VA_ARGS__)
void ssa_sprint_addr(int level, char *str, size_t str_size,
		     enum ssa_addr_type addr_type, uint8_t *addr, size_t addr_size);
void ssa_log_options(void);

enum ssa_svc_state {
	SSA_STATE_IDLE,
	SSA_STATE_JOINING,
	SSA_STATE_FATAL_ERROR,
	SSA_STATE_ORPHAN,
	SSA_STATE_HAVE_PARENT,
	SSA_STATE_CONNECTING,
	SSA_STATE_CONNECTED,
	SSA_STATE_NO_BACKUP,
	SSA_STATE_HAVE_BACKUP
};

/*
 * Nested locking order: dest -> ep, dest -> port
 */

struct ssa_dest {
//	uint8_t                address[ACM_MAX_ADDRESS]; /* keep first */
//	char                   name[ACM_MAX_ADDRESS];
	struct ibv_ah          *ah;
	struct ibv_ah_attr     av;
//	struct ibv_path_record path;
//	uint64_t               req_id;
//	DLIST_ENTRY            req_queue;
//	uint32_t               remote_qpn;
//	lock_t                 lock;
//	enum acm_state         state;
	atomic_t               refcnt;
//	uint8_t                addr_type;
};

struct ssa_port {
	struct ssa_device	*dev;
//	DLIST_ENTRY		ep_list;
//	lock_t			lock;
	int			mad_portid;
	int			mad_agentid;
//	struct ssa_dest		sa_dest;	// needed?
	enum ibv_port_state	state;
	int			gid_cnt;
	uint16_t		pkey_cnt;
	uint16_t		lid;
	uint16_t		lid_mask;
	uint8_t			port_num;
};

struct ssa_device {
	struct ssa_class	*ssa;
	struct ibv_context      *verbs;
	uint64_t                guid;
	char			name[IBV_SYSFS_NAME_MAX];
	size_t			port_size;
	int                     port_cnt;
	struct ssa_port         *port;
};

struct ssa_ep {
	struct ssa_port       *port;
//	struct ibv_cq         *cq;
//	struct ibv_qp         *qp;
//	struct ibv_mr         *mr;
//	uint8_t               *recv_bufs;
	DLIST_ENTRY           entry;
	uint16_t              pkey_index;
	uint16_t              pkey;
//	lock_t                lock;
//	DLIST_ENTRY           req_queue;
	enum ssa_svc_state    state;
};

int ssa_open_devices(struct ssa_class *ssa);
//void ssa_activate_devices(void);
void ssa_close_devices(struct ssa_class *ssa);

struct ssa_class {
	struct ssa_device	*dev;
	int			dev_cnt;
	size_t			dev_size;
	size_t			port_size;
};

static inline struct ssa_port *ssa_dev_port(struct ssa_device *dev, int index)
{
	return (struct ssa_port *) ((void *) dev->port + dev->port_size * index);
}

static inline struct ssa_device *ssa_dev(struct ssa_class *ssa, int index)
{
	return (struct ssa_device *) ((void *) ssa->dev + ssa->dev_size * index);
}

int ssa_init(struct ssa_class *ssa, size_t dev_size, size_t port_size);
void ssa_cleanup(struct ssa_class *ssa);

/* clients currently setup to connect over TCP sockets */
struct ssa_client {
//	lock_t   lock;   /* acquire ep lock first */
	int	 sock;
	int      index;
	atomic_t refcnt;
};

struct ssa_request {
	struct acm_client *client;
	DLIST_ENTRY       entry;
//	struct ssa_msg    msg;
};

void ssa_init_server();
int ssa_listen();
void ssa_disconnect_client(struct ssa_client *client);
