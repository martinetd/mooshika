/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet.ocre@cea.fr //TODO: use a real mail?
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    trans_rdma.c
 * \brief   rdma helper
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */

#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <inttypes.h>	//uint*_t
#include <errno.h>	//ENOMEM
#include <sys/socket.h> //sockaddr
#include <pthread.h>	//pthread_* (think it's included by another one)
#include <semaphore.h>  //sem_* (is it a good idea to mix sem and pthread_cond/mutex?)
#include <arpa/inet.h>  //inet_ntop
#include <netinet/in.h> //sock_addr_in
#include <unistd.h>	//fcntl
#include <fcntl.h>	//fcntl
#include <poll.h>	//poll

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"


/**
 * \struct libercat_ctx
 * Context data we can use during recv/send callbacks
 */
struct libercat_ctx {
	int used;			/**< 0 if we can use it for a new recv/send */
	uint32_t pos;			/**< current position inside our own buffer. 0 <= pos <= len */
	uint32_t len;			/**< size of our own buffer */
	struct rdmactx *next;		/**< next context */
	libercat_data_t *data;
	ctx_callback_t callback;
	struct ibv_sge sge;
	union {
		struct ibv_recv_wr rwr;
		struct ibv_send_wr wwr;
	} wr;
	void *callback_arg;
};

/* UTILITY FUNCTIONS */

/**
 * libercat_reg_mr: registers memory for rdma use (almost the same as ibv_reg_mr)
 *
 * @param trans   [IN]
 * @param memaddr [IN] the address to register
 * @param size    [IN] the size of the area to register
 * @param access  [IN] the access to grants to the mr (e.g. IBV_ACCESS_LOCAL_WRITE)
 *
 * @return a pointer to the mr if registered correctly or NULL on failure
 */

struct ibv_mr *libercat_reg_mr(libercat_trans_t *trans, void *memaddr, size_t size, int access) {
	return ibv_reg_mr(trans->pd, memaddr, size, access);
}

/**
 * libercat_reg_mr: deregisters memory for rdma use (exactly ibv_dereg_mr)
 *
 * @param mr [INOUT] the mr to deregister
 *
 * @return 0 on success, errno value on failure
 */
int libercat_dereg_mr(struct ibv_mr *mr) {
	return ibv_dereg_mr(mr);
}

/**
 * libercat_make_rloc: makes a rkey to send it for remote host use
 * 
 * @param mr   [IN] the mr in which the addr belongs
 * @param addr [IN] the addr to give
 * @param size [IN] the size to allow (hint)
 *
 * @return a pointer to the rkey on success, NULL on failure.
 */
libercat_rloc_t *libercat_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size) {
	libercat_rloc_t *rloc;
	rloc = malloc(sizeof(libercat_rloc_t));
	if (!rloc) {
		ERROR_LOG("Out of memory!");
		return NULL;
	}

	rloc->raddr = addr;
	rloc->rkey = mr->rkey;
	rloc->size = size;

	return rloc;
}

void libercat_print_devinfo(libercat_trans_t *trans) {
	struct ibv_device_attr device_attr;
	ibv_query_device(trans->cm_id->verbs, &device_attr);
	uint64_t node_guid = ntohll(device_attr.node_guid);
	printf("guid: %04x:%04x:%04x:%04x\n",
		(unsigned) (node_guid >> 48) & 0xffff,
		(unsigned) (node_guid >> 32) & 0xffff,
		(unsigned) (node_guid >> 16) & 0xffff,
		(unsigned) (node_guid >>  0) & 0xffff);
}

/* INIT/SHUTDOWN FUNCTIONS */

/**
 * libercat_cma_event_handler: handles addr/route resolved events (client side) and disconnect (everyone)
 *
 */
static int libercat_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event) {
	int ret = 0;
	libercat_trans_t *trans = cma_id->context;

	INFO_LOG("cma_event type %s", rdma_event_str(event->event));

	if (trans->bad_recv_wr) {
		ERROR_LOG("Something was bad on that recv");
	}
	if (trans->bad_send_wr) {
		ERROR_LOG("Something was bad on that send");
	}

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		INFO_LOG("ADDR_RESOLVED");
		pthread_mutex_lock(&trans->lock);
		trans->state = LIBERCAT_ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, trans->timeout);
		if (ret) {
			trans->state = LIBERCAT_ERROR;
			ERROR_LOG("rdma_resolve_route failed");
			pthread_cond_signal(&trans->cond);
		}
		pthread_mutex_unlock(&trans->lock);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		INFO_LOG("ROUTE_RESOLVED");
		pthread_mutex_lock(&trans->lock);
		trans->state = LIBERCAT_ROUTE_RESOLVED;
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		INFO_LOG("ESTABLISHED");
		pthread_mutex_lock(&trans->lock);
		trans->state = LIBERCAT_CONNECTED;
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		ERROR_LOG("cma event %s, error %d",
			rdma_event_str(event->event), event->status);
		pthread_mutex_lock(&trans->lock);
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		ERROR_LOG("DISCONNECT EVENT...");

		ret = -1;

		trans->state = LIBERCAT_CLOSED;
		if (trans->disconnect_callback)
			trans->disconnect_callback(trans);
		pthread_mutex_lock(&trans->lock);
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		ERROR_LOG("cma detected device removal!!!!");
		ret = -1;
		break;

	default:
		INFO_LOG("unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}

	return ret;
}

/**
 * libercat_cm_thread: thread function which waits for new connection events and gives them to handler (then ack the event)
 *
 */
static void *libercat_cm_thread(void *arg) {
	libercat_trans_t *trans = arg;
	struct rdma_cm_event *event;
	int ret;

	while (1) {
		ret = rdma_get_cm_event(trans->event_channel, &event);
		if (ret) {
			ret = errno;
			ERROR_LOG("rdma_get_cm_event failed: %d. Stopping event watcher thread", ret);
			break;
		}
		ret = libercat_cma_event_handler(event->id, event);
		rdma_ack_cm_event(event);
		if (ret) {
			if (trans->state != LIBERCAT_CLOSED)
				ERROR_LOG("something happened in cma_event_handler. Stopping event watcher thread");
			break;
		}
	}

	pthread_exit(NULL);
}

/**
 * libercat_cq_event_handler: completion queue event handler.
 * marks contexts back out of use and calls the appropriate callbacks for each kind of event
 *
 * @return 0 on success, work completion status if not 0
 */
static int libercat_cq_event_handler(libercat_trans_t *trans) {
	struct ibv_wc wc;
	libercat_ctx_t* ctx;
	int ret;

	while ((ret = ibv_poll_cq(trans->cq, 1, &wc)) == 1) {
		ret = 0;

		if (trans->bad_recv_wr) {
			ERROR_LOG("Something was bad on that recv");
		}
		if (trans->bad_send_wr) {
			ERROR_LOG("Something was bad on that send");
		}
		if (wc.status) {
			if (trans->state != LIBERCAT_CLOSED)
				ERROR_LOG("cq completion failed status: %s (%d)", ibv_wc_status_str(wc.status), wc.status);
			return wc.status;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
		case IBV_WC_RDMA_WRITE:
		case IBV_WC_RDMA_READ:
			INFO_LOG("WC_SEND/RDMA_WRITE/RDMA_READ: %d", wc.opcode);

			ctx = (libercat_ctx_t *)wc.wr_id;
			if (ctx->callback)
				((ctx_callback_t)ctx->callback)(trans, ctx->callback_arg);

			pthread_mutex_lock(&trans->lock);
			ctx->used = 0;
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
			break;

		case IBV_WC_RECV:
			INFO_LOG("WC_RECV");

			if (wc.wc_flags & IBV_WC_WITH_IMM) {
				//FIXME ctx->data->imm_data = ntohl(wc.imm_data);
				ERROR_LOG("imm_data: %d", ntohl(wc.imm_data));
			}

			ctx = (libercat_ctx_t *)wc.wr_id;
			ctx->data->size = wc.byte_len;
			if (ctx->callback)
				((ctx_callback_t)ctx->callback)(trans, ctx->callback_arg);

			pthread_mutex_lock(&trans->lock);
			ctx->used = 0;
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
			break;

		default:
			ERROR_LOG("unknown opcode: %d", wc.opcode);
			return -1;
		}
	}

	return 0;
}

/**
 * libercat_cq_thread: thread function which waits for new completion events and gives them to handler (then ack the event)
 *
 */
static void *libercat_cq_thread(void *arg) {
	libercat_trans_t *trans = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;

	//make get_cq_event_nonblocking for poll
	int flags;
	flags = fcntl(trans->comp_channel->fd, F_GETFL);
	ret = fcntl(trans->comp_channel->fd, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		ERROR_LOG("Failed to make the comp channel nonblock");
		pthread_exit(NULL);
	}

	struct pollfd comp_pollfd;
	comp_pollfd.fd = trans->comp_channel->fd;
	comp_pollfd.events = POLLIN;
	comp_pollfd.revents = 0;

	while (trans->state != LIBERCAT_CLOSED) {
		ret = poll(&comp_pollfd, 1, 100);

		if (ret == 0)
			continue;

		if (ret == -1) {
			ERROR_LOG("poll failed");
			break;
		}

		ret = ibv_get_cq_event(trans->comp_channel, &ev_cq, &ev_ctx);
		if (ret) {
			ERROR_LOG("ibv_get_cq_event failed, leaving thread.");
			break;
		}
		if (ev_cq != trans->cq) {
		 	ERROR_LOG("Unknown cq, leaving thread.");
			break;
		}

		ret = ibv_req_notify_cq(trans->cq, 0);
		if (ret) {
			ERROR_LOG("ibv_req_notify_cq failed: %d. Leaving thread.", ret);
			break;
		}

		ret = libercat_cq_event_handler(trans);
		ibv_ack_cq_events(trans->cq, 1);
		if (ret) {
			if (trans->state != LIBERCAT_CLOSED)
				ERROR_LOG("something went wrong with our cq_event_handler, leaving thread after ack.");
			break;
		}
	}

	pthread_exit(NULL);
}


/**
 * libercat_destroy_buffer
 *
 * @param trans [INOUT]
 *
 * @return void even if some stuff here can fail //FIXME?
 */
static void libercat_destroy_buffer(libercat_trans_t *trans) {
	if (trans->send_buf) {
		free(trans->send_buf);
		trans->send_buf = NULL;
	}
	if (trans->recv_buf) {
		free(trans->recv_buf);
		trans->recv_buf = NULL;
	}
}

/**
 * libercat_destroy_qp: destroys all qp-related stuff for us
 *
 * @param trans [INOUT]
 *
 * @return void, even if the functions _can_ fail we choose to ignore it. //FIXME?
 */
static void libercat_destroy_qp(libercat_trans_t *trans) {
	if (trans->qp) {
		ibv_destroy_qp(trans->qp);
		trans->qp = NULL;
	}
	if (trans->cq) {
		ibv_destroy_cq(trans->cq);
		trans->cq = NULL;
	}
	if (trans->comp_channel) {
		ibv_destroy_comp_channel(trans->comp_channel);
		trans->comp_channel = NULL;
	}
	if (trans->pd) {
		ibv_dealloc_pd(trans->pd);
		trans->pd = NULL;
	}
}


/**
 * libercat_destroy_trans: disconnects and free trans data
 *
 * @param ptrans [INOUT] pointer to the trans to destroy
 */
void libercat_destroy_trans(libercat_trans_t **ptrans) {

	libercat_trans_t *trans = *ptrans;

	if (trans) {
		pthread_mutex_lock(&trans->lock);
		if (trans->cm_id)
			rdma_disconnect(trans->cm_id);

		while (trans->state != LIBERCAT_CLOSED) {
			ERROR_LOG("we're not closed yet, waiting for disconnect_event");
			pthread_cond_wait(&trans->cond, &trans->lock);
		}
		pthread_mutex_unlock(&trans->lock);

		if (trans->cm_thread)
			pthread_join(trans->cm_thread, NULL);
		if (trans->cq_thread)
			pthread_join(trans->cq_thread, NULL); //FIXME: cf. libercat_cq_thread's fixme, it's possible this never ends

		// these two functions do the proper if checks
		libercat_destroy_buffer(trans);
		libercat_destroy_qp(trans);

		if (trans->cm_id) {
			rdma_destroy_id(trans->cm_id);
			trans->cm_id = NULL;
		}
		// event channel is shared between all children, so don't close it unless it's its own.
		if (((!trans->server) || (trans->state == LIBERCAT_LISTENING)) && trans->event_channel) {
			rdma_destroy_event_channel(trans->event_channel);
			trans->event_channel = NULL;
		}

		//FIXME check if it is init. if not should just return EINVAL but.. lock.__lock, cond.__lock might work.
		pthread_mutex_unlock(&trans->lock);
		pthread_mutex_destroy(&trans->lock);
		pthread_cond_destroy(&trans->cond);

		free(trans);
		*ptrans = NULL;
	}
}

/**
 * libercat_init: part of the init that's the same for client and server
 *
 * @param ptrans [INOUT]
 * @param attr   [IN]    attributes to set parameters in ptrans. attr->addr must be set, others can be either 0 or sane values.
 *
 * @return 0 on success, errno value on failure
 */
int libercat_init(libercat_trans_t **ptrans, libercat_trans_attr_t *attr) {
	int ret;

	libercat_trans_t *trans;

	*ptrans = malloc(sizeof(libercat_trans_t));
	if (!*ptrans) {
		ERROR_LOG("Out of memory");
		return ENOMEM;
	}

	trans=*ptrans;

	memset(trans, 0, sizeof(libercat_trans_t));

	trans->event_channel = rdma_create_event_channel();
	if (!trans->event_channel) {
		ret = errno;
		ERROR_LOG("create_event_channel failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(&trans);
		return ret;
	}

	ret = rdma_create_id(trans->event_channel, &trans->cm_id, trans, RDMA_PS_TCP);
	if (ret) {
		ret = errno;
		ERROR_LOG("create_id failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(&trans);
		return ret;
	}

	trans->state = LIBERCAT_INIT;

	if (!attr->addr.sa_stor.ss_family) { //FIXME: do a proper check?
		ERROR_LOG("address has to be defined");
		return EDESTADDRREQ;
	}
	trans->addr.sa_stor = attr->addr.sa_stor;

	trans->server = attr->server;
	trans->timeout = attr->timeout   ? attr->timeout  : 3000000; // in ms
	trans->sq_depth = attr->sq_depth ? attr->sq_depth : 10;
	trans->rq_depth = attr->rq_depth ? attr->rq_depth : 50;
	trans->disconnect_callback = attr->disconnect_callback;

	if (attr->pd)
		trans->pd = attr->pd;

	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(&trans);
		return ret;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(&trans);
		return ret;
	}

	return 0;
}

/**
 * libercat_create_qp: create a qp associated with a trans
 *
 * @param trans [INOUT]
 * @param cm_id [IN]
 *
 * @ret 0 on success, errno value on error
 */
static int libercat_create_qp(libercat_trans_t *trans, struct rdma_cm_id *cm_id) {
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = trans->sq_depth;
	init_attr.cap.max_recv_wr = trans->rq_depth;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.cap.max_inline_data = 0; // change if IMM
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = trans->cq;
	init_attr.recv_cq = trans->cq;
	init_attr.sq_sig_all = 1;

	if (rdma_create_qp(cm_id, trans->pd, &init_attr)) {
		ret = errno;
		ERROR_LOG("rdma_create_qp: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->qp = cm_id->qp;
	return 0;
}

/**
 * libercat_setup_qp: setups pd, qp an' stuff
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int libercat_setup_qp(libercat_trans_t *trans) {
	int ret;

	INFO_LOG("trans: %p", trans);

	if (!trans->pd)
		trans->pd = ibv_alloc_pd(trans->cm_id->verbs);
	if (!trans->pd) {
		ret = errno;
		ERROR_LOG("ibv_alloc_pd failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->comp_channel = ibv_create_comp_channel(trans->cm_id->verbs);
	if (!trans->comp_channel) {
		ret = errno;
		ERROR_LOG("ibv_create_comp_channel failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_qp(trans);
		return ret;
	}

	trans->cq = ibv_create_cq(trans->cm_id->verbs, trans->sq_depth + trans->rq_depth,
				  trans, trans->comp_channel, 0);
	if (!trans->cq) {
		ret = errno;
		ERROR_LOG("ibv_create_cq failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_qp(trans);
		return ret;
	}

	ret = ibv_req_notify_cq(trans->cq, 0);
	if (ret) {
		ERROR_LOG("ibv_req_notify_cq failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_qp(trans);
		return ret;
	}

	ret = libercat_create_qp(trans, trans->cm_id);
	if (ret) {
		ERROR_LOG("our own create_qp failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_qp(trans);
		return ret;
	}

	INFO_LOG("created qp %p", trans->qp);
	return 0;
}


/**
 * libercat_setup_buffer
 */
static int libercat_setup_buffer(libercat_trans_t *trans) {
	trans->recv_buf = malloc(trans->rq_depth * sizeof(libercat_ctx_t));
	if (!trans->recv_buf) {
		ERROR_LOG("couldn't malloc trans->recv_buf");
		return ENOMEM;
	}
	memset(trans->recv_buf, 0, trans->rq_depth * sizeof(libercat_ctx_t));

	trans->send_buf = malloc(trans->sq_depth * sizeof(libercat_ctx_t));
	if (!trans->send_buf) {
		ERROR_LOG("couldn't malloc trans->send_buf");
		return ENOMEM;
	}
	memset(trans->send_buf, 0, trans->sq_depth * sizeof(libercat_ctx_t));

	return 0;
}

/**
 * libercat_bind_server
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
int libercat_bind_server(libercat_trans_t *trans) {
	int ret;


	if (!trans) {
		ERROR_LOG("trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server <= 0) {
		ERROR_LOG("Must be on server side to call this function");
		return EINVAL;
	}


	char str[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &trans->addr.sa_in.sin_addr, str, INET_ADDRSTRLEN);
	INFO_LOG("addr: %s, port: %d", str, ntohs(trans->addr.sa_in.sin_port));

	ret = rdma_bind_addr(trans->cm_id, &trans->addr.sa);
	if (ret) {
		ret = errno;

		return ret;
	}

	ret = rdma_listen(trans->cm_id, trans->server);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_listen failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	trans->state = LIBERCAT_LISTENING;

	return 0;
}


static libercat_trans_t *clone_trans(libercat_trans_t *listening_trans, struct rdma_cm_id *cm_id) {
	libercat_trans_t *trans = malloc(sizeof(libercat_trans_t));
	int ret;

	if (!trans) {
		ERROR_LOG("malloc failed");
		return NULL;
	}

	memcpy(trans, listening_trans, sizeof(libercat_trans_t));

	trans->cm_id = cm_id;
	trans->cm_id->context = trans;

	memset(&trans->lock, 0, sizeof(pthread_mutex_t));
	memset(&trans->cond, 0, sizeof(pthread_cond_t));

	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(&trans);
		return NULL;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(&trans);
		return NULL;
	}

	return trans;
}

/**
 * libercat_accept: does the real connection acceptance //FIXME it's still called by accept_one, either make it static again or don't call it in accept_one...
 *
 * @param trans [IN]
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_finalize_accept(libercat_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.private_data = NULL;
	conn_param.private_data_len = 0;
	conn_param.rnr_retry_count = 10;

	pthread_mutex_lock(&trans->lock);
	ret = rdma_accept(trans->cm_id, &conn_param);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_accept failed: %s (%d)", strerror(ret), ret);
		return ret;
	}


	while (trans->state != LIBERCAT_CONNECTED) {
		pthread_cond_wait(&trans->cond, &trans->lock);
	}

	pthread_mutex_unlock(&trans->lock);

	return 0;
}

/**
 * libercat_start_cm_thread: starts cm event thread for server side in case there's no accept_one idling
 *
 * @param trans [IN]
 *
 * @return same as pthread_create (0 on success)
 */
int libercat_start_cm_thread(libercat_trans_t *trans) {
	pthread_attr_t attr_thr;

	/* Init for thread parameter (mostly for scheduling) */
	if(pthread_attr_init(&attr_thr) != 0)
		ERROR_LOG("can't init pthread's attributes");

	if(pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM) != 0)
		ERROR_LOG("can't set pthread's scope");

	if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE) != 0)
		ERROR_LOG("can't set pthread's join state");

	return pthread_create(&trans->cm_thread, &attr_thr, libercat_cm_thread, trans);
}

/**
 * libercat_accept_one: given a listening trans, waits till one connection is requested and accepts it
 *
 * @param rdma_connection [IN] the mother trans
 *
 * @return a new trans for the child on success, NULL on failure
 */
libercat_trans_t *libercat_accept_one(libercat_trans_t *rdma_connection) { //TODO make it return an int an' use trans as argument

	//TODO: timeout?

	struct rdma_cm_event *event;
	struct rdma_cm_id *cm_id;
	libercat_trans_t *trans = NULL;
	int ret;

	while (!trans) {
		ret = rdma_get_cm_event(rdma_connection->event_channel, &event);
		if (ret) {
			ret=errno;
			ERROR_LOG("rdma_get_cm_event failed: %s (%d)", strerror(ret), ret);
			return NULL;
		}

		cm_id = (struct rdma_cm_id *)event->id;

		switch (event->event) {
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			INFO_LOG("CONNECT_REQUEST");
			trans = clone_trans(rdma_connection, cm_id);
			break;

		case RDMA_CM_EVENT_ESTABLISHED:
			INFO_LOG("ESTABLISHED");
			trans = cm_id->context;
			pthread_mutex_lock(&trans->lock);
			trans->state = LIBERCAT_CONNECTED;
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
			trans = NULL;
			break;

		case RDMA_CM_EVENT_DISCONNECTED:
			INFO_LOG("DISCONNECTED");
			trans = cm_id->context;
			pthread_mutex_lock(&trans->lock);
			trans->state = LIBERCAT_CLOSED;
			if (trans->disconnect_callback)
				trans->disconnect_callback(trans);
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
			trans = NULL;
			break;

		default:
			INFO_LOG("unhandled event: %s", rdma_event_str(event->event));
		}
		rdma_ack_cm_event(event);
	}
	if (trans) {
		libercat_setup_qp(trans); //FIXME: check return codes //FIXME: decide what to do with half-init connection requests that failed...
		libercat_setup_buffer(trans);
		pthread_attr_t attr_thr;

		/* Init for thread parameter (mostly for scheduling) */
		if(pthread_attr_init(&attr_thr) != 0)
			ERROR_LOG("can't init pthread's attributes");

		if(pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM) != 0)
			ERROR_LOG("can't set pthread's scope");

		if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE) != 0)
			ERROR_LOG("can't set pthread's join state");

		pthread_create(&trans->cq_thread, &attr_thr, libercat_cq_thread, trans);
	}
	return trans;
}

/**
 * libercat_bind_client: resolve addr and route for the client and waits till it's done
 * (the route and pthread_cond_signal is done in the cm thread)
 *
 */
static int libercat_bind_client(libercat_trans_t *trans) {
	int ret;

	pthread_mutex_lock(&trans->lock);

	ret = rdma_resolve_addr(trans->cm_id, NULL, (struct sockaddr*) &trans->addr, trans->timeout);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_resolve_addr failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	pthread_cond_wait(&trans->cond, &trans->lock);
	pthread_mutex_unlock(&trans->lock);

	return 0;
}

/**
 * libercat_connect_client: does the actual connection to the server //FIXME: do we want to remove the static to allow for post_recv before connecting?
 *
 */
int libercat_finalize_connect(libercat_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.rnr_retry_count = 10;
	conn_param.retry_count = 10;

	pthread_mutex_lock(&trans->lock);

	ret = rdma_connect(trans->cm_id, &conn_param);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_connect failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	pthread_cond_wait(&trans->cond, &trans->lock);
	pthread_mutex_unlock(&trans->lock);

	if (trans->state != LIBERCAT_CONNECTED) {
		ERROR_LOG("trans not in CONNECTED state as expected");
		return ENOTCONN;
	}

	return 0;
}

/**
 * libercat_connect: connects a client to a server
 *
 * @param trans [INOUT] trans must be init first
 *
 * @return 0 on success, the value of errno on error 
 */
int libercat_connect(libercat_trans_t *trans) {

	if (!trans) {
		ERROR_LOG("trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server) {
		ERROR_LOG("Must be on client side to call this function");
		return EINVAL;
	}

	pthread_attr_t attr_thr;

	/* Init for thread parameter (mostly for scheduling) */
	if(pthread_attr_init(&attr_thr) != 0)
		ERROR_LOG("can't init pthread's attributes");

	if(pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM) != 0)
		ERROR_LOG("can't set pthread's scope");

	if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE) != 0)
		ERROR_LOG("can't set pthread's join state");

	pthread_create(&trans->cm_thread, &attr_thr, libercat_cm_thread, trans);

	libercat_bind_client(trans);
	libercat_setup_qp(trans);
	libercat_setup_buffer(trans);

	pthread_create(&trans->cq_thread, &attr_thr, libercat_cq_thread, trans);

	return 0;
}



/**
 * libercat_post_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans        [IN]
 * @param pdata        [OUT] the data buffer to be filled with received data //FIXME: isn't a *data enough?
 * @param mr           [IN]  the mr in which the data lives
 * @param callback     [IN]  function that'll be called when done
 * @param callback_arg [IN]  argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_post_recv(libercat_trans_t *trans, libercat_data_t **pdata, struct ibv_mr *mr, ctx_callback_t callback, void* callback_arg) {
	INFO_LOG("posting recv");
	libercat_ctx_t *rctx;
	int i, ret;

	pthread_mutex_lock(&trans->lock);

	do {
		for (i = 0, rctx = trans->recv_buf;
		     i < trans->rq_depth;
		     i++, rctx++)
			if (!rctx->used)
				break;

		if (i == trans->rq_depth) {
			INFO_LOG("Waiting for cond");
			pthread_cond_wait(&trans->cond, &trans->lock);
		}

	} while ( i == trans->rq_depth );
	INFO_LOG("got a free context");

	pthread_mutex_unlock(&trans->lock);

	rctx->used = 1;
	rctx->len = (*pdata)->max_size;
	rctx->pos = 0;
	rctx->next = NULL;
	rctx->callback = callback;
	rctx->callback_arg = callback_arg;
	rctx->data = *pdata;

	rctx->sge.addr = (uintptr_t) rctx->data->data;
	rctx->sge.length = rctx->len;
	rctx->sge.lkey = mr->lkey;
	rctx->wr.rwr.next = NULL;
	rctx->wr.rwr.wr_id = (uint64_t)rctx;
	rctx->wr.rwr.sg_list = &rctx->sge;
	rctx->wr.rwr.num_sge = 1;

	ret = ibv_post_recv(trans->qp, &rctx->wr.rwr, &trans->bad_recv_wr);
	if (ret) {
		ERROR_LOG("ibv_post_recv failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

static int libercat_post_send_generic(libercat_trans_t *trans, enum ibv_wr_opcode opcode, libercat_data_t *data, struct ibv_mr *mr, libercat_rloc_t *rloc, ctx_callback_t callback, void* callback_arg) {
	INFO_LOG("posting a send with op %d", opcode);
	libercat_ctx_t *wctx;
	int i, ret;

	// opcode-specific checks:
	if (opcode == IBV_WR_RDMA_WRITE || opcode == IBV_WR_RDMA_READ) {
		if (!rloc) {
			ERROR_LOG("Cannot do rdma without a remote location!");
			return EINVAL;
		}
		if (data->size > rloc->size) {
			ERROR_LOG("trying to send or read a buffer bigger than the remote buffer (shall we truncate?)");
			return EMSGSIZE;
		}
	} else if (opcode == IBV_WR_SEND || opcode == IBV_WR_SEND_WITH_IMM) {
	} else {
		ERROR_LOG("unsupported op code: %d", opcode);
		return EINVAL;
	}


	pthread_mutex_lock(&trans->lock);

	do {
		for (i = 0, wctx = (libercat_ctx_t *)trans->send_buf;
		     i < trans->sq_depth;
		     i++, wctx = (libercat_ctx_t *)((uint8_t *)wctx + sizeof(libercat_ctx_t)))
			if (!wctx->used)
				break;

		if (i == trans->sq_depth) {
			INFO_LOG("waiting for cond");
			pthread_cond_wait(&trans->cond, &trans->lock);
		}

	} while ( i == trans->sq_depth );
	INFO_LOG("got a free context");

	pthread_mutex_unlock(&trans->lock);

	wctx->used = 1;
	wctx->len = data->size;
	wctx->pos = 0;
	wctx->next = NULL;
	wctx->callback = callback;
	wctx->callback_arg = callback_arg;
	wctx->data = data;

	wctx->sge.addr = (uintptr_t)wctx->data->data;
	wctx->sge.length = wctx->len;
	wctx->sge.lkey = mr->lkey;
	wctx->wr.wwr.next = NULL;
	wctx->wr.wwr.wr_id = (uint64_t)wctx;
	wctx->wr.wwr.opcode = opcode;
//FIXME	wctx->wr.wwr.imm_data = htonl(data->imm_data);
	wctx->wr.wwr.send_flags = IBV_SEND_SIGNALED;
	wctx->wr.wwr.sg_list = &wctx->sge;
	wctx->wr.wwr.num_sge = 1;
	if (rloc) {
		wctx->wr.wwr.wr.rdma.rkey = rloc->rkey;
		wctx->wr.wwr.wr.rdma.remote_addr = rloc->raddr;
	}

	ret = ibv_post_send(trans->qp, &wctx->wr.wwr, &trans->bad_send_wr);
	if (ret) {
		ERROR_LOG("ibv_post_send failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * Post a send buffer.
 *
 * @param trans        [IN]
 * @param data         [IN] the data buffer to be sent
 * @param mr           [IN] the mr in which the data lives
 * @param callback     [IN] function that'll be called when done
 * @param callback_arg [IN] argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_post_send(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr, ctx_callback_t callback, void* callback_arg) {
	return libercat_post_send_generic(trans, IBV_WR_SEND, data, mr, NULL, callback, callback_arg);
}

/**
 * libercat_wait_callback: send/recv callback that just unlocks a mutex.
 *
 */
static void libercat_wait_callback(libercat_trans_t *trans, void *arg) {
	pthread_mutex_t *lock = arg;
	pthread_mutex_unlock(lock);
}

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * Generally a bad idea to use that one unless only that one is used.
 *
 * @param trans [IN]
 * @param pdata [OUT] the data buffer to be filled with the received data
 * @param mr    [IN]  the memory region in which data lives
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_wait_recv(libercat_trans_t *trans, libercat_data_t **pdata, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = libercat_post_recv(trans, pdata, mr, libercat_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans [IN]
 * @param data  [IN] the data to send
 * @param mr    [IN] the memory region in which data lives
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_wait_send(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = libercat_post_send(trans, data, mr, libercat_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

// callbacks would all be run in a big send/recv_thread


// server specific:


int libercat_post_read(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr, libercat_rloc_t *rloc, ctx_callback_t callback, void* callback_arg) {
	return libercat_post_send_generic(trans, IBV_WR_RDMA_READ, data, mr, rloc, callback, callback_arg);
}

int libercat_post_write(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr, libercat_rloc_t *rloc, ctx_callback_t callback, void* callback_arg) {
	return libercat_post_send_generic(trans, IBV_WR_RDMA_WRITE, data, mr, rloc, callback, callback_arg);
}

int libercat_wait_read(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr, libercat_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = libercat_post_read(trans, data, mr, rloc, libercat_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


int libercat_wait_write(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr, libercat_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = libercat_post_write(trans, data, mr, rloc, libercat_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


// client specific:
int libercat_write_request(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size); // = ask for libercat_write server side ~= libercat_read
int libercat_read_request(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size); // = ask for libercat_read server side ~= libercat_write
