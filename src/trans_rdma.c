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
#include "mooshika.h"


/**
 * \struct msk_ctx
 * Context data we can use during recv/send callbacks
 */
struct msk_ctx {
	int used;			/**< 0 if we can use it for a new recv/send */
	uint32_t pos;			/**< current position inside our own buffer. 0 <= pos <= len */
	struct rdmactx *next;		/**< next context */
	msk_data_t *pdata;
	ctx_callback_t callback;
	ctx_callback_t err_callback;
	union {
		struct ibv_recv_wr rwr;
		struct ibv_send_wr wwr;
	} wr;
	void *callback_arg;
	struct ibv_sge sg_list[0]; 		/**< this is actually an array. note that when you malloc you have to add its size */
};

/* UTILITY FUNCTIONS */

/**
 * msk_reg_mr: registers memory for rdma use (almost the same as ibv_reg_mr)
 *
 * @param trans   [IN]
 * @param memaddr [IN] the address to register
 * @param size    [IN] the size of the area to register
 * @param access  [IN] the access to grants to the mr (e.g. IBV_ACCESS_LOCAL_WRITE)
 *
 * @return a pointer to the mr if registered correctly or NULL on failure
 */

struct ibv_mr *msk_reg_mr(msk_trans_t *trans, void *memaddr, size_t size, int access) {
	return ibv_reg_mr(trans->pd, memaddr, size, access);
}

/**
 * msk_reg_mr: deregisters memory for rdma use (exactly ibv_dereg_mr)
 *
 * @param mr [INOUT] the mr to deregister
 *
 * @return 0 on success, errno value on failure
 */
int msk_dereg_mr(struct ibv_mr *mr) {
	return ibv_dereg_mr(mr);
}

/**
 * msk_make_rloc: makes a rkey to send it for remote host use
 * 
 * @param mr   [IN] the mr in which the addr belongs
 * @param addr [IN] the addr to give
 * @param size [IN] the size to allow (hint)
 *
 * @return a pointer to the rkey on success, NULL on failure.
 */
msk_rloc_t *msk_make_rloc(struct ibv_mr *mr, uint64_t addr, uint32_t size) {
	msk_rloc_t *rloc;
	rloc = malloc(sizeof(msk_rloc_t));
	if (!rloc) {
		ERROR_LOG("Out of memory!");
		return NULL;
	}

	rloc->raddr = addr;
	rloc->rkey = mr->rkey;
	rloc->size = size;

	return rloc;
}

void msk_print_devinfo(msk_trans_t *trans) {
	struct ibv_device_attr device_attr;
	ibv_query_device(trans->cm_id->verbs, &device_attr);
	uint64_t node_guid = ntohll(device_attr.node_guid);
	printf("guid: %04x:%04x:%04x:%04x\n",
		(unsigned) (node_guid >> 48) & 0xffff,
		(unsigned) (node_guid >> 32) & 0xffff,
		(unsigned) (node_guid >> 16) & 0xffff,
		(unsigned) (node_guid >>  0) & 0xffff);
}

/**
 * msk_create_thread: Simple wrapper around pthread_create
 */
#define THREAD_STACK_SIZE 2116488
static inline int msk_create_thread(pthread_t *thread,
              void *(*start_routine)(void*), void *arg) {

	pthread_attr_t attr;
	int ret;

	/* Init for thread parameter (mostly for scheduling) */
	if ((ret = pthread_attr_init(&attr)) != 0) {
		ERROR_LOG("can't init pthread's attributes: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM)) != 0) {
		ERROR_LOG("can't set pthread's scope: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) != 0) {
		ERROR_LOG("can't set pthread's join state: %s (%d)", strerror(ret), ret);
		return ret;
	}

	if ((ret = pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE)) != 0) {
		ERROR_LOG("can't set pthread's stack size: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return pthread_create(thread, &attr, start_routine, arg);
}

/* INIT/SHUTDOWN FUNCTIONS */

/**
 * msk_cma_event_handler: handles addr/route resolved events (client side) and disconnect (everyone)
 *
 */
static int msk_cma_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event) {
	int ret = 0;
	msk_trans_t *trans = cm_id->context;

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
		trans->state = MSK_ADDR_RESOLVED;
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		INFO_LOG("ROUTE_RESOLVED");
		pthread_mutex_lock(&trans->lock);
		trans->state = MSK_ROUTE_RESOLVED;
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		INFO_LOG("ESTABLISHED");
		pthread_mutex_lock(&trans->lock);
		trans->state = MSK_CONNECTED;
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		INFO_LOG("CONNECT_REQUEST");
		//even if the cm_id is new, trans is the good parent's trans.
		int i = 0;
		pthread_mutex_lock(&trans->lock);

		//FIXME don't run through this stupidely and remember last index written to and last index read, i.e. use as a queue
		/* Find an empty connection request slot */
		for (i = 0; i < trans->server; i++)
			if (!trans->conn_requests[i])
				break;

		if (i == trans->server) {
			pthread_mutex_unlock(&trans->lock);
			ERROR_LOG("Could not pile up new connection requests' cm_id!");
			ret = ENOBUFS;
			break;
		}

		// write down new cm_id and signal accept handler there's stuff to do
		trans->conn_requests[i] = cm_id;
		pthread_cond_broadcast(&trans->cond);
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
		ret = ECONNREFUSED;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		ERROR_LOG("DISCONNECT EVENT...");

		ret = ECONNRESET;

		pthread_mutex_lock(&trans->lock);
		trans->state = MSK_CLOSED;
		pthread_cond_signal(&trans->cond);
		pthread_mutex_unlock(&trans->lock);
		if (trans->disconnect_callback)
			trans->disconnect_callback(trans);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		ERROR_LOG("cma detected device removal!!!!");
		ret = ENODEV;
		break;

	default:
		INFO_LOG("unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}

	return ret;
}

/**
 * msk_cm_thread: thread function which waits for new connection events and gives them to handler (then ack the event)
 *
 */
static void *msk_cm_thread(void *arg) {
	msk_trans_t *trans = arg;
	struct rdma_cm_event *event;
	int ret;

	//make get_cq_event_nonblocking for poll
	int flags;
	flags = fcntl(trans->event_channel->fd, F_GETFL);
	ret = fcntl(trans->event_channel->fd, F_SETFL, flags | O_NONBLOCK);
	if (ret < 0) {
		ERROR_LOG("Failed to make the comp channel nonblock");
		pthread_exit(NULL);
	}

	struct pollfd event_pollfd;
	event_pollfd.fd = trans->event_channel->fd;
	event_pollfd.events = POLLIN;
	event_pollfd.revents = 0;

	while (trans->state != MSK_CLOSED) {
		ret = poll(&event_pollfd, 1, 100);

		if (ret == 0)
			continue;

		if (ret == -1) {
			if (trans->state != MSK_CLOSED)
				ERROR_LOG("poll failed");
			break;
		}

		if (!trans->event_channel) {
			if (trans->state != MSK_CLOSED)
				ERROR_LOG("no event channel? :D");
			break;
		}

		ret = rdma_get_cm_event(trans->event_channel, &event);
		if (ret) {
			ret = errno;
			ERROR_LOG("rdma_get_cm_event failed: %d. Stopping event watcher thread", ret);
			break;
		}
		ret = msk_cma_event_handler(event->id, event);
		rdma_ack_cm_event(event);
		if (ret && (trans->state != MSK_LISTENING || trans == event->id->context)) {
			if (trans->state != MSK_CLOSED)
				ERROR_LOG("something happened in cma_event_handler. Stopping event watcher thread");
			break;
		}
	}

	pthread_exit(NULL);
}

/**
 * msk_cq_event_handler: completion queue event handler.
 * marks contexts back out of use and calls the appropriate callbacks for each kind of event
 *
 * @return 0 on success, work completion status if not 0
 */
static int msk_cq_event_handler(msk_trans_t *trans) {
	struct ibv_wc wc;
	msk_ctx_t* ctx;
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
			if (trans->state != MSK_CLOSED)
				ERROR_LOG("cq completion failed status: %s (%d)", ibv_wc_status_str(wc.status), wc.status);

			ctx = (msk_ctx_t *)wc.wr_id;
			if (ctx && ctx->err_callback)
				ctx->err_callback(trans, ctx->pdata, ctx->callback_arg);

                        pthread_mutex_lock(&trans->lock);
	                ctx->used = 0;
                        pthread_cond_broadcast(&trans->cond);
	                pthread_mutex_unlock(&trans->lock);

			return wc.status;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
		case IBV_WC_RDMA_WRITE:
		case IBV_WC_RDMA_READ:
			INFO_LOG("WC_SEND/RDMA_WRITE/RDMA_READ: %d", wc.opcode);

			ctx = (msk_ctx_t *)wc.wr_id;
			if (ctx->callback)
				((ctx_callback_t)ctx->callback)(trans, ctx->pdata, ctx->callback_arg);

			pthread_mutex_lock(&trans->lock);
			ctx->used = 0;
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
			break;

		case IBV_WC_RECV:
			INFO_LOG("WC_RECV");

			if (wc.wc_flags & IBV_WC_WITH_IMM) {
				//FIXME ctx->pdata->imm_data = ntohl(wc.imm_data);
				ERROR_LOG("imm_data: %d", ntohl(wc.imm_data));
			}

			ctx = (msk_ctx_t *)wc.wr_id;
			
			// fill all the sizes in case of multiple sge
			int len = wc.byte_len;
			msk_data_t *pdata = ctx->pdata;
			while (pdata && len > pdata->max_size) {
				pdata->size = pdata->max_size;
				len -= pdata->max_size;
				pdata = pdata->next;
			}
			if (pdata)
				pdata->size = len;

			if (ctx->callback)
				((ctx_callback_t)ctx->callback)(trans, ctx->pdata, ctx->callback_arg);

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
 * msk_cq_thread: thread function which waits for new completion events and gives them to handler (then ack the event)
 *
 */
static void *msk_cq_thread(void *arg) {
	msk_trans_t *trans = arg;
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

	while (trans->state != MSK_CLOSED) {
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

		ret = msk_cq_event_handler(trans);
		ibv_ack_cq_events(trans->cq, 1);
		if (ret) {
			if (trans->state != MSK_CLOSED) {
				ERROR_LOG("something went wrong with our cq_event_handler, leaving thread after ack.");
				pthread_mutex_lock(&trans->lock);
				trans->state = MSK_ERROR;
				pthread_cond_signal(&trans->cond);
				pthread_mutex_unlock(&trans->lock);
			}
			break;
		}
	}

	pthread_exit(NULL);
}


/**
 * msk_destroy_buffer
 *
 * @param trans [INOUT]
 *
 * @return void even if some stuff here can fail //FIXME?
 */
static void msk_destroy_buffer(msk_trans_t *trans) {
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
 * msk_destroy_qp: destroys all qp-related stuff for us
 *
 * @param trans [INOUT]
 *
 * @return void, even if the functions _can_ fail we choose to ignore it. //FIXME?
 */
static void msk_destroy_qp(msk_trans_t *trans) {
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
 * msk_destroy_trans: disconnects and free trans data
 *
 * @param ptrans [INOUT] pointer to the trans to destroy
 */
void msk_destroy_trans(msk_trans_t **ptrans) {

	msk_trans_t *trans = *ptrans;

	if (trans) {
		/* FIXME: what to do on error? */
		if (trans->state == MSK_CONNECTED || trans->state == MSK_CLOSED) {
			pthread_mutex_lock(&trans->lock);
			if (trans->cm_id && trans->cm_id->verbs)
				rdma_disconnect(trans->cm_id);

			while (trans->state != MSK_CLOSED && trans->state != MSK_LISTENING && trans->state != MSK_ERROR) {
				ERROR_LOG("we're not closed yet, waiting for disconnect_event");
				pthread_cond_wait(&trans->cond, &trans->lock);
			}
			pthread_mutex_unlock(&trans->lock);
		}

		if (trans->cm_id) {
			rdma_destroy_id(trans->cm_id);
			trans->cm_id = NULL;
		}

		// event channel is shared between all children, so don't close it unless it's its own.
		if (((!trans->server) || (trans->state == MSK_LISTENING)) && trans->event_channel) {
			trans->state = MSK_CLOSED;
			rdma_destroy_event_channel(trans->event_channel);
			trans->event_channel = NULL;
		}

		if (trans->cm_thread)
			pthread_join(trans->cm_thread, NULL);
		if (trans->cq_thread)
			pthread_join(trans->cq_thread, NULL);

		// these two functions do the proper if checks
		msk_destroy_buffer(trans);
		msk_destroy_qp(trans);

		//FIXME check if it is init. if not should just return EINVAL but.. lock.__lock, cond.__lock might work.
		pthread_mutex_unlock(&trans->lock);
		pthread_mutex_destroy(&trans->lock);
		pthread_cond_destroy(&trans->cond);

		free(trans);
		*ptrans = NULL;
	}
}

/**
 * msk_init: part of the init that's the same for client and server
 *
 * @param ptrans [INOUT]
 * @param attr   [IN]    attributes to set parameters in ptrans. attr->addr must be set, others can be either 0 or sane values.
 *
 * @return 0 on success, errno value on failure
 */
int msk_init(msk_trans_t **ptrans, msk_trans_attr_t *attr) {
	int ret;

	msk_trans_t *trans;

	if (!ptrans || !attr) {
		ERROR_LOG("Invalid argument");
		return EINVAL;
	}

	*ptrans = malloc(sizeof(msk_trans_t));
	if (!*ptrans) {
		ERROR_LOG("Out of memory");
		return ENOMEM;
	}

	trans=*ptrans;

	memset(trans, 0, sizeof(msk_trans_t));

	trans->event_channel = rdma_create_event_channel();
	if (!trans->event_channel) {
		ret = errno;
		ERROR_LOG("create_event_channel failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return ret;
	}

	ret = rdma_create_id(trans->event_channel, &trans->cm_id, trans, RDMA_PS_TCP);
	if (ret) {
		ret = errno;
		ERROR_LOG("create_id failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return ret;
	}

	trans->state = MSK_INIT;

	if (!attr->addr.sa_stor.ss_family) { //FIXME: do a proper check?
		ERROR_LOG("address has to be defined");
		return EDESTADDRREQ;
	}
	trans->addr.sa_stor = attr->addr.sa_stor;

	trans->server = attr->server;
	trans->timeout = attr->timeout   ? attr->timeout  : 3000000; // in ms
	trans->sq_depth = attr->sq_depth ? attr->sq_depth : 10;
	trans->max_send_sge = attr->max_send_sge ? attr->max_send_sge : 1;
	trans->rq_depth = attr->rq_depth ? attr->rq_depth : 50;
	trans->max_recv_sge = attr->max_recv_sge ? attr->max_recv_sge : 1;
	trans->disconnect_callback = attr->disconnect_callback;

	if (attr->pd)
		trans->pd = attr->pd;

	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return ret;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return ret;
	}

	return 0;
}

/**
 * msk_create_qp: create a qp associated with a trans
 *
 * @param trans [INOUT]
 * @param cm_id [IN]
 *
 * @ret 0 on success, errno value on error
 */
static int msk_create_qp(msk_trans_t *trans, struct rdma_cm_id *cm_id) {
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = trans->sq_depth;
	init_attr.cap.max_recv_wr = trans->rq_depth;
	init_attr.cap.max_recv_sge = trans->max_recv_sge;
	init_attr.cap.max_send_sge = trans->max_send_sge;
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
 * msk_setup_qp: setups pd, qp an' stuff
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
static int msk_setup_qp(msk_trans_t *trans) {
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
		msk_destroy_qp(trans);
		return ret;
	}

	trans->cq = ibv_create_cq(trans->cm_id->verbs, trans->sq_depth + trans->rq_depth,
				  trans, trans->comp_channel, 0);
	if (!trans->cq) {
		ret = errno;
		ERROR_LOG("ibv_create_cq failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	ret = ibv_req_notify_cq(trans->cq, 0);
	if (ret) {
		ERROR_LOG("ibv_req_notify_cq failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	ret = msk_create_qp(trans, trans->cm_id);
	if (ret) {
		ERROR_LOG("our own create_qp failed: %s (%d)", strerror(ret), ret);
		msk_destroy_qp(trans);
		return ret;
	}

	INFO_LOG("created qp %p", trans->qp);
	return 0;
}


/**
 * msk_setup_buffer
 */
static int msk_setup_buffer(msk_trans_t *trans) {
	trans->recv_buf = malloc(trans->rq_depth * (sizeof(msk_ctx_t) + trans->max_recv_sge * sizeof(struct ibv_sge)));
	if (!trans->recv_buf) {
		ERROR_LOG("couldn't malloc trans->recv_buf");
		return ENOMEM;
	}
	memset(trans->recv_buf, 0, trans->rq_depth * (sizeof(msk_ctx_t) + trans->max_recv_sge * sizeof(struct ibv_sge)));

	trans->send_buf = malloc(trans->sq_depth * (sizeof(msk_ctx_t) + trans->max_send_sge * sizeof(struct ibv_sge)));
	if (!trans->send_buf) {
		ERROR_LOG("couldn't malloc trans->send_buf");
		return ENOMEM;
	}
	memset(trans->send_buf, 0, trans->sq_depth * (sizeof(msk_ctx_t) + trans->max_send_sge * sizeof(struct ibv_sge)));

	return 0;
}

/**
 * msk_bind_server
 *
 * @param trans [INOUT]
 *
 * @return 0 on success, errno value on failure
 */
int msk_bind_server(msk_trans_t *trans) {
	int ret;

	if (!trans || trans->state != MSK_INIT) {
		ERROR_LOG("trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server <= 0) {
		ERROR_LOG("Must be on server side to call this function");
		return EINVAL;
	}


	//FIXME: if (debug)?
	char str[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &trans->addr.sa_in.sin_addr, str, INET_ADDRSTRLEN);
	INFO_LOG("addr: %s, port: %d", str, ntohs(trans->addr.sa_in.sin_port));

        trans->conn_requests = malloc(trans->server * sizeof(struct rdma_cm_id*));
        if (!trans->conn_requests) {
                ERROR_LOG("Could not allocate conn_requests buffer");
                return ENOMEM;
        }

        memset(trans->conn_requests, 0, trans->server * sizeof(struct rdma_cm_id*));



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

	trans->state = MSK_LISTENING;

	if ((ret = msk_create_thread(&trans->cm_thread, msk_cm_thread, trans))) {
		ERROR_LOG("msk_create_thread failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return 0;
}


static msk_trans_t *clone_trans(msk_trans_t *listening_trans, struct rdma_cm_id *cm_id) {
	msk_trans_t *trans = malloc(sizeof(msk_trans_t));
	int ret;

	if (!trans) {
		ERROR_LOG("malloc failed");
		return NULL;
	}

	memcpy(trans, listening_trans, sizeof(msk_trans_t));

	trans->cm_id = cm_id;
	trans->cm_id->context = trans;
	trans->state = MSK_CONNECT_REQUEST;

	// we don't want to close cm thread when destroying the child
	trans->cm_thread = 0;

	memset(&trans->lock, 0, sizeof(pthread_mutex_t));
	memset(&trans->cond, 0, sizeof(pthread_cond_t));

	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		msk_destroy_trans(&trans);
		return NULL;
	}

	return trans;
}

/**
 * msk_finalize_accept: does the real connection acceptance and wait for other side to be ready
 *
 * @param trans [IN]
 *
 * @return 0 on success, the value of errno on error
 */
int msk_finalize_accept(msk_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	if (!trans || trans->state != MSK_CONNECT_REQUEST) {
		ERROR_LOG("trans isn't from a connection request?");
		return EINVAL;
	}

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


	while (trans->state != MSK_CONNECTED) {
		pthread_cond_wait(&trans->cond, &trans->lock);
		INFO_LOG("Got a cond, state: %i", trans->state);
	}

	pthread_mutex_unlock(&trans->lock);

	return 0;
}

/**
 * msk_accept_one: given a listening trans, waits till one connection is requested and accepts it
 *
 * @param trans [IN] the parent trans
 *
 * @return a new trans for the child on success, NULL on failure
 */
msk_trans_t *msk_accept_one(msk_trans_t *trans) { //TODO make it return an int an' use trans as argument

	//TODO: timeout?

	struct rdma_cm_id *cm_id = NULL;
	msk_trans_t *child_trans = NULL;
	int i;

	if (!trans || trans->state != MSK_LISTENING) {
		ERROR_LOG("trans isn't listening (after bind_server)?");
		return NULL;
	}

	pthread_mutex_lock(&trans->lock);
	while (!cm_id) {
		/* See if one of the slots has been taken */
		for (i = 0; i < trans->server; i++)
			if (trans->conn_requests[i])
				break;

		if (i == trans->server) {
			INFO_LOG("Waiting for a connection to come in");
			pthread_cond_wait(&trans->cond, &trans->lock);
		} else {
			cm_id = trans->conn_requests[i];
			trans->conn_requests[i] = NULL;
		}
	}
	pthread_mutex_unlock(&trans->lock);

	INFO_LOG("Got a connection request - creating child");

	child_trans = clone_trans(trans, cm_id);

	if (child_trans) {
		if (msk_setup_qp(child_trans)) {
			msk_destroy_trans(&child_trans);
			return NULL;
		}
		if (msk_setup_buffer(child_trans)) {
			msk_destroy_trans(&child_trans);
			return NULL;
		}

		if (msk_create_thread(&child_trans->cq_thread, msk_cq_thread, child_trans)) {
			msk_destroy_trans(&child_trans);
			return NULL;
		}
	}
	return child_trans;
}

/**
 * msk_bind_client: resolve addr and route for the client and waits till it's done
 * (the route and pthread_cond_signal is done in the cm thread)
 *
 */
static int msk_bind_client(msk_trans_t *trans) {
	int ret;

	pthread_mutex_lock(&trans->lock);

	ret = rdma_resolve_addr(trans->cm_id, NULL, (struct sockaddr*) &trans->addr, trans->timeout);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_resolve_addr failed: %s (%d)", strerror(ret), ret);
		return ret;
	}


	while (trans->state != MSK_ADDR_RESOLVED) {
	       	pthread_cond_wait(&trans->cond, &trans->lock);
		INFO_LOG("Got a cond, state: %i", trans->state);
	}

	ret = rdma_resolve_route(trans->cm_id, trans->timeout);
	if (ret) {
		trans->state = MSK_ERROR;
		ERROR_LOG("rdma_resolve_route failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	while (trans->state != MSK_ROUTE_RESOLVED) {
	       	pthread_cond_wait(&trans->cond, &trans->lock);
		INFO_LOG("Got a cond, state: %i", trans->state);
	}

	pthread_mutex_unlock(&trans->lock);

	return 0;
}

/**
 * msk_finalize_connect: tells the other side we're ready to receive stuff (does the actual rdma_connect) and waits for its ack
 *
 * @param trans [IN]
 *
 * @return 0 on success, errno value on failure
 */
int msk_finalize_connect(msk_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	if (!trans || trans->state != MSK_ROUTE_RESOLVED) {
		ERROR_LOG("trans isn't half-connected?");
		return EINVAL;
	}


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

	while (trans->state != MSK_CONNECTED) {
		pthread_cond_wait(&trans->cond, &trans->lock);
		INFO_LOG("Got a cond, state: %i", trans->state);
	}

	pthread_mutex_unlock(&trans->lock);

	return 0;
}

/**
 * msk_connect: connects a client to a server
 *
 * @param trans [INOUT] trans must be init first
 *
 * @return 0 on success, the value of errno on error 
 */
int msk_connect(msk_trans_t *trans) {
	int ret;

	if (!trans || trans->state != MSK_INIT) {
		ERROR_LOG("trans must be initialized first!");
		return EINVAL;
	}

	if (trans->server) {
		ERROR_LOG("Must be on client side to call this function");
		return EINVAL;
	}

	if ((ret = msk_create_thread(&trans->cm_thread, msk_cm_thread, trans)))
		return ret;

	if ((ret = msk_bind_client(trans)))
		return ret;
	if ((ret = msk_setup_qp(trans)))
		return ret;
	if ((ret = msk_setup_buffer(trans)))
		return ret;

	if ((ret = msk_create_thread(&trans->cq_thread, msk_cq_thread, trans)))
		return ret;

	return 0;
}



/**
 * msk_post_n_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans        [IN]
 * @param pdata        [OUT] the data buffer to be filled with received data
 * @param num_sge      [IN]  the number of elements in pdata to register
 * @param mr           [IN]  the mr in which the data lives
 * @param callback     [IN]  function that'll be called when done
 * @param err_callback [IN]  function that'll be called on error
 * @param callback_arg [IN]  argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_recv(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	INFO_LOG("posting recv");
	msk_ctx_t *rctx;
	int i, ret;

	if (!trans || (trans->state != MSK_CONNECTED && trans->state != MSK_ROUTE_RESOLVED && trans->state != MSK_CONNECT_REQUEST)) {
       		ERROR_LOG("trans isn't connected?");
		return EINVAL;
	}


	pthread_mutex_lock(&trans->lock);

	do {
		for (i = 0, rctx = trans->recv_buf;
		     i < trans->rq_depth;
		     i++, rctx = (msk_ctx_t*)((uint8_t*)rctx + sizeof(msk_ctx_t) + trans->max_recv_sge*sizeof(struct ibv_sge)))
			if (!rctx->used)
				break;

		if (i == trans->rq_depth) {
			INFO_LOG("Waiting for cond");
			pthread_cond_wait(&trans->cond, &trans->lock);
		}

	} while ( i == trans->rq_depth );
	INFO_LOG("got a free context");
	rctx->used = 1;

	pthread_mutex_unlock(&trans->lock);

	rctx->pos = 0;
	rctx->next = NULL;
	rctx->callback = callback;
	rctx->err_callback = err_callback;
	rctx->callback_arg = callback_arg;
	rctx->pdata = pdata;

	for (i=0; i < num_sge; i++) {
		if (!pdata) {
			ERROR_LOG("You said to recv %d elements (num_sge), but we only found %d! Not requesting.", num_sge, i);
			return EINVAL;
		} 
		rctx->sg_list[i].addr = (uintptr_t) pdata->data;
		INFO_LOG("addr: %lx\n", rctx->sg_list->addr);
		rctx->sg_list[i].length = pdata->max_size;
		rctx->sg_list[i].lkey = mr->lkey;
		pdata = pdata->next; 
	}

	rctx->wr.rwr.next = NULL;
	rctx->wr.rwr.wr_id = (uint64_t)rctx;
	rctx->wr.rwr.sg_list = rctx->sg_list;
	rctx->wr.rwr.num_sge = num_sge;

	ret = ibv_post_recv(trans->qp, &rctx->wr.rwr, &trans->bad_recv_wr);
	if (ret) {
		ERROR_LOG("ibv_post_recv failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

static int msk_post_send_generic(msk_trans_t *trans, enum ibv_wr_opcode opcode, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	INFO_LOG("posting a send with op %d", opcode);
	msk_ctx_t *wctx;
	int i, ret;
	uint32_t totalsize = 0;

	if (!trans || trans->state != MSK_CONNECTED) {
       		ERROR_LOG("trans isn't connected?");
		return EINVAL;
	}

	// opcode-specific checks:
	if (opcode == IBV_WR_RDMA_WRITE || opcode == IBV_WR_RDMA_READ) {
		if (!rloc) {
			ERROR_LOG("Cannot do rdma without a remote location!");
			return EINVAL;
		}
	} else if (opcode == IBV_WR_SEND || opcode == IBV_WR_SEND_WITH_IMM) {
	} else {
		ERROR_LOG("unsupported op code: %d", opcode);
		return EINVAL;
	}


	pthread_mutex_lock(&trans->lock);

	do {
		for (i = 0, wctx = (msk_ctx_t *)trans->send_buf;
		     i < trans->sq_depth;
		     i++, wctx = (msk_ctx_t*)((uint8_t*)wctx + sizeof(msk_ctx_t) + trans->max_send_sge*sizeof(struct ibv_sge)))
			if (!wctx->used)
				break;

		if (i == trans->sq_depth) {
			INFO_LOG("waiting for cond");
			pthread_cond_wait(&trans->cond, &trans->lock);
		}

	} while ( i == trans->sq_depth );
	INFO_LOG("got a free context");
	wctx->used = 1;

	pthread_mutex_unlock(&trans->lock);

	wctx->pos = 0;
	wctx->next = NULL;
	wctx->callback = callback;
	wctx->err_callback = err_callback;
	wctx->callback_arg = callback_arg;
	wctx->pdata = pdata;

	for (i=0; i < num_sge; i++) {
		if (!pdata) {
			ERROR_LOG("You said to send %d elements (num_sge), but we only found %d! Not sending.", num_sge, i);
			// or send up to previous one? It's probably an error though...
			return EINVAL;
		} 
		if (pdata->size == 0) {
			num_sge = i; // only send up to previous sg, do we want to warn about this?
			break;
		}

		wctx->sg_list[i].addr = (uintptr_t)pdata->data;
		INFO_LOG("addr: %lx\n", wctx->sg_list[i].addr);
		wctx->sg_list[i].length = pdata->size;
		wctx->sg_list[i].lkey = mr->lkey;
		totalsize += pdata->size;

		pdata = pdata->next;
	}

	if (rloc && totalsize > rloc->size) {
		ERROR_LOG("trying to send or read a buffer bigger than the remote buffer (shall we truncate?)");
		return EMSGSIZE;
	}

	wctx->wr.wwr.next = NULL;
	wctx->wr.wwr.wr_id = (uint64_t)wctx;
	wctx->wr.wwr.opcode = opcode;
//FIXME	wctx->wr.wwr.imm_data = htonl(data->imm_data);
	wctx->wr.wwr.send_flags = IBV_SEND_SIGNALED;
	wctx->wr.wwr.sg_list = wctx->sg_list;
	wctx->wr.wwr.num_sge = num_sge;
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
 * @param pdata        [IN] the data buffer to be sent
 * @param num_sge      [IN] the number of elements in pdata to send
 * @param mr           [IN] the mr in which the data lives
 * @param callback     [IN] function that'll be called when done
 * @param err_callback [IN] function that'll be called on error
 * @param callback_arg [IN] argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int msk_post_n_send(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_SEND, pdata, num_sge, mr, NULL, callback, err_callback, callback_arg);
}

/**
 * msk_wait_callback: send/recv callback that just unlocks a mutex.
 *
 */
static void msk_wait_callback(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	pthread_mutex_t *lock = arg;
	pthread_mutex_unlock(lock);
}

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * Generally a bad idea to use that one unless only that one is used.
 *
 * @param trans   [IN]
 * @param pdata   [OUT] the data buffer to be filled with the received data
 * @param num_sge [IN]  the number of elements in pdata to register
 * @param mr      [IN]  the memory region in which data lives
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_recv(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_recv(trans, pdata, num_sge, mr, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans   [IN]
 * @param pdata   [IN] the data to send
 * @param num_sge [IN] the number of elements in pdata to send
 * @param mr      [IN] the memory region in which data lives
 *
 * @return 0 on success, the value of errno on error
 */
int msk_wait_n_send(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_send(trans, pdata, num_sge, mr, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}

// callbacks would all be run in a big send/recv_thread


// server specific:


int msk_post_n_read(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_RDMA_READ, pdata, num_sge, mr, rloc, callback, err_callback, callback_arg);
}

int msk_post_n_write(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc, ctx_callback_t callback, ctx_callback_t err_callback, void* callback_arg) {
	return msk_post_send_generic(trans, IBV_WR_RDMA_WRITE, pdata, num_sge, mr, rloc, callback, err_callback, callback_arg);
}

int msk_wait_n_read(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_read(trans, pdata, num_sge, mr, rloc, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


int msk_wait_n_write(msk_trans_t *trans, msk_data_t *pdata, int num_sge, struct ibv_mr *mr, msk_rloc_t *rloc) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	pthread_mutex_lock(&lock);
	ret = msk_post_n_write(trans, pdata, num_sge, mr, rloc, msk_wait_callback, msk_wait_callback, &lock);

	if (!ret) {
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);
		pthread_mutex_destroy(&lock);
	}

	return ret;
}


struct sockaddr *msk_get_peer_addr(struct rdma_cm_id *id) {
	return rdma_get_peer_addr(id);
}

struct sockaddr *msk_get_local_addr(struct rdma_cm_id *id) {
	return rdma_get_local_addr(id);
}

uint16_t msk_get_src_port(struct rdma_cm_id *id) {
	return rdma_get_src_port(id);
}

uint16_t msk_get_dst_port(struct rdma_cm_id *id) {
	return rdma_get_dst_port(id);
}



