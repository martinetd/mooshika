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

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

/* UTILITY FUNCTIONS */

struct ibv_mr *libercat_reg_mr(libercat_trans_t *trans, void *memaddr, size_t size, int access) {
	return ibv_reg_mr(trans->pd, memaddr, size, access); // todo: mr->context = trans;
}

int libercat_dereg_mr(struct ibv_mr *mr) {
	return ibv_dereg_mr(mr);
}

libercat_rloc_t *libercat_make_rkey(uint64_t addr, struct ibv_mr *mr, uint32_t size) {
	libercat_rloc_t *rkey;
	rkey = malloc(sizeof(libercat_rloc_t));
	if (!rkey) {
		ERROR_LOG("Out of memory!");
		return NULL;
	}

	rkey->rmemaddr = addr;
	rkey->rkey = mr->rkey;
	rkey->size = size;

	return rkey;
}

/* INIT/SHUTDOWN FUNCTIONS */

/**
 * libercat_cma_event_handler
 *
 * handles _client-side_ addr/route resolved events and disconnect
 * is not used at all by the server
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


static void *libercat_cm_thread(void *arg) {
	libercat_trans_t *trans = arg;
	struct rdma_cm_event *event;
	int ret;

	while (1) {
		ret = rdma_get_cm_event(trans->event_channel, &event);
		if (ret) {
			ret = errno;
			ERROR_LOG("rdma_get_cm_event failed: %d. Stopping event watcher thread", ret);
			pthread_exit(NULL); //TODO: give the value to main thread somewhere? continue a few times?
		}
		ret = libercat_cma_event_handler(event->id, event);
		rdma_ack_cm_event(event);
		if (ret) {
			ERROR_LOG("something happened in cma_event_handler. Stopping event watcher thread");
			pthread_exit(NULL);
		}
	}

}


static int libercat_cq_event_handler(libercat_trans_t *trans) {
	struct ibv_wc wc;
	libercat_ctx_t* ctx;
	int ret;

	if ((ret = ibv_poll_cq(trans->cq, 1, &wc)) == 1) {
		ret = 0;

		if (trans->bad_recv_wr) {
			ERROR_LOG("Something was bad on that recv");
		}
		if (trans->bad_send_wr) {
			ERROR_LOG("Something was bad on that send");
		}
		if (wc.status) {
			ERROR_LOG("cq completion failed status: %s (%d)", ibv_wc_status_str(wc.status), wc.status);
			return -1;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
			INFO_LOG("WC_SEND");

			ctx = (libercat_ctx_t *)wc.wr_id;
			((ctx_callback_t)ctx->callback)(trans, ctx->callback_arg);

			pthread_mutex_lock(&trans->lock);
			ctx->used = 0;
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
			break;

		case IBV_WC_RECV:
			INFO_LOG("WC_RECV");

			ctx = (libercat_ctx_t *)wc.wr_id;
			ctx->data->size = wc.byte_len;
			((ctx_callback_t)ctx->callback)(trans, ctx->callback_arg);

			pthread_mutex_lock(&trans->lock);
			ctx->used = 0;
			pthread_cond_broadcast(&trans->cond);
			pthread_mutex_unlock(&trans->lock);
			break;

		case IBV_WC_RDMA_WRITE:
			INFO_LOG("WC_RDMA_WRITE");
			break;

		case IBV_WC_RDMA_READ:
			INFO_LOG("WC_RDMA_READ");
			break;

		default:
			ERROR_LOG("unknown opcode: %d", wc.opcode);
			return -1;
		}
	}

	return 0;
}

static void *libercat_cq_thread(void *arg) {
	libercat_trans_t *trans = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;

	while (1) {
		pthread_testcancel();

		ret = ibv_get_cq_event(trans->comp_channel, &ev_cq, &ev_ctx);
		if (ret) {
			ERROR_LOG("ibv_get_cq_event failed, leaving thread.");
			pthread_exit(NULL);
		}
		if (ev_cq != trans->cq) {
			ERROR_LOG("Unknown cq, leaving thread.");
			pthread_exit(NULL);
		}

		ret = ibv_req_notify_cq(trans->cq, 0);
		if (ret) {
			ERROR_LOG("ibv_req_notify_cq failed: %d. Leaving thread.", ret);
			pthread_exit(NULL);
		}

		ret = libercat_cq_event_handler(trans);
		ibv_ack_cq_events(trans->cq, 1);
		if (ret) {
			ERROR_LOG("something went wrong with our cq_event_handler, leaving thread after ack.");
			pthread_exit(NULL);
		}
	}

}


/**
 * libercat_destroy_buffer
 *
 * @param trans [INOUT]
 *
 * @return void even if some stuff here can fail //FIXME?
 */
static void libercat_destroy_buffer(libercat_trans_t *trans) {
	if (trans->send_buf)
		free(trans->send_buf);
	if (trans->recv_buf)
		free(trans->recv_buf);
}

/**
 * libercat_destroy_qp: destroys all qp-related stuff for us
 *
 * @param trans [INOUT]
 *
 * @return void, even if the functions _can_ fail we choose to ignore it. //FIXME?
 */
static void libercat_destroy_qp(libercat_trans_t *trans) {
	if (trans->qp)
		ibv_destroy_qp(trans->qp);
	if (trans->cq)
		ibv_destroy_cq(trans->cq);
	if (trans->comp_channel)
		ibv_destroy_comp_channel(trans->comp_channel);
	if (trans->pd)
		ibv_dealloc_pd(trans->pd);
}


/**
 * libercat_destroy_trans: disconnects and free trans data
 *
 * @param trans [INOUT] the trans to destroy
 */
void libercat_destroy_trans(libercat_trans_t *trans) {

	if (trans->cm_id)
		rdma_disconnect(trans->cm_id);
	if (trans->cq_thread)
		pthread_join(trans->cq_thread, NULL);

	// these two functions do the proper if checks
	libercat_destroy_buffer(trans);
	libercat_destroy_qp(trans);

	if (trans->cm_id)
		rdma_destroy_id(trans->cm_id);
	if (trans->event_channel)
		rdma_destroy_event_channel(trans->event_channel);

	//FIXME check if it is init. if not should just return EINVAL but.. lock.__lock, cond.__lock might work.
	pthread_mutex_destroy(&trans->lock);
	pthread_cond_destroy(&trans->cond);

	free(trans);
}

/**
 * libercat_init: part of the init that's the same for client and server
 *
 * @param ptrans [INOUT]
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
		libercat_destroy_trans(trans);
		return ret;
	}

	ret = rdma_create_id(trans->event_channel, &trans->cm_id, trans, RDMA_PS_TCP);
	if (ret) {
		ret = errno;
		ERROR_LOG("create_id failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(trans);
		return ret;
	}

	trans->state = LIBERCAT_INIT;

	if (!attr->addr.ss_family) { //FIXME: do a proper check?
		ERROR_LOG("address has to be defined");
		return EDESTADDRREQ;
	}
	trans->addr = attr->addr;

	trans->timeout = attr->timeout ?: 3000000; // in ms
	trans->sq_depth = attr->sq_depth ?: 10;
	trans->rq_depth = attr->rq_depth ?: 50;
	trans->num_accept = attr->num_accept ?: 10;

	ret = pthread_mutex_init(&trans->lock, NULL);
	if (ret) {
		ERROR_LOG("pthread_mutex_init failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(trans);
		return ret;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(trans);
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
	init_attr.cap.max_inline_data = 64;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = trans->cq;
	init_attr.recv_cq = trans->cq;

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
 * @param addr [IN] contains the full address (i.e. both ip and port)
 *
 * @return 0 on success, errno value on failure
 */
int libercat_bind_server(libercat_trans_t *trans) {
	int ret;


	if (!trans) {
		ERROR_LOG("trans must be initialized first!");
		return -1;
	}

	if (!trans->addr.ss_family) {
		ERROR_LOG("trans.addr must be set");
		return -1;
	}

	trans->server = 1;


	char str[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, &((struct sockaddr_in*)&trans->addr)->sin_addr, str, INET_ADDRSTRLEN);
	INFO_LOG("addr: %s, port: %d", str, ntohs(((struct sockaddr_in*)&trans->addr)->sin_port));

	ret = rdma_bind_addr(trans->cm_id, (struct sockaddr*) &trans->addr);
	if (ret) {
		ret = errno;

		return ret;
	}

	ret = rdma_listen(trans->cm_id, trans->num_accept);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_listen failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

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
		libercat_destroy_trans(trans);
		return NULL;
	}
	ret = pthread_cond_init(&trans->cond, NULL);
	if (ret) {
		ERROR_LOG("pthread_cond_init failed: %s (%d)", strerror(ret), ret);
		libercat_destroy_trans(trans);
		return NULL;
	}

	return trans;
}

int libercat_accept(libercat_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.private_data = NULL;
	conn_param.private_data_len = 0;
	ret = rdma_accept(trans->cm_id, &conn_param);
	if (ret) {
		ret = errno;
		ERROR_LOG("rdma_accept failed: %s (%d)", strerror(ret), ret);
		return ret;
	}

	return 0;
}

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
			break;

		case RDMA_CM_EVENT_DISCONNECTED:
			INFO_LOG("DISCONNECTED");
			libercat_destroy_trans((libercat_trans_t *)cm_id->context);
			break;

		default:
			INFO_LOG("unhandled event: %s", rdma_event_str(event->event));
		}
	}
	if (trans) {
		libercat_setup_qp(trans);
		libercat_setup_buffer(trans);
		pthread_create(&trans->cm_thread, NULL, libercat_cm_thread, trans);
		pthread_create(&trans->cq_thread, NULL, libercat_cq_thread, trans);
		libercat_accept(trans);
	}
	return trans;
}

/*
 * libercat_bind_client
 *
 *
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

static int libercat_connect_client(libercat_trans_t *trans) {
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof(struct rdma_conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
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
		return -1;
	}

	return 0;
}

// do we want create/destroy + listen/shutdown, or can both be done in a single call?
// if second we could have create/destroy shared with client, but honestly there's not much to share...
// client
int libercat_connect(libercat_trans_t *trans) {

	if (!trans) {
		ERROR_LOG("trans must be initialized first!");
		return -1;
	}

	if (!trans->addr.ss_family) {
		ERROR_LOG("trans.addr must be set");
		return -1;
	}

	trans->server = 0;

	pthread_create(&trans->cm_thread, NULL, libercat_cm_thread, trans);

	libercat_bind_client(trans);
	libercat_setup_qp(trans);
	libercat_setup_buffer(trans);

	pthread_create(&trans->cq_thread, NULL, libercat_cq_thread, trans);
	libercat_connect_client(trans);

	return 0;
}



/**
 * libercat_post_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans    [IN]
 * @param ibv_mr   [IN] max size we can receive
 * @param callback [IN] function that'll be called with the received data
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_post_recv(libercat_trans_t *trans, libercat_data_t **pdata, struct ibv_mr *mr, ctx_callback_t callback, void* callback_arg) {
	INFO_LOG("posted recv");
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
//			pthread_cond_wait(&trans->cond, &trans->lock);
		}

	} while ( i == trans->rq_depth );
	INFO_LOG("got a free context");

	pthread_mutex_unlock(&trans->lock);

	rctx->wc_op = IBV_WC_RECV;
	rctx->used = 1;
	rctx->len = (*pdata)->max_size;
	rctx->pos = 0;
	rctx->next = NULL;
	rctx->callback = (void *)callback;
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

/**
 * Post a send buffer.
 *
 * data must be inside the mr!
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_post_send(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr, ctx_callback_t callback, void* callback_arg) {
	INFO_LOG("posted send");
	libercat_ctx_t *wctx;
	int i, ret;

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

	wctx->wc_op = IBV_WC_SEND;
	wctx->used = 1;
	wctx->len = data->size;
	wctx->pos = 0;
	wctx->next = NULL;
	wctx->callback = (void *)callback;
	wctx->callback_arg = callback_arg;
	wctx->data = data;

	wctx->sge.addr = (uintptr_t)wctx->data->data;
	wctx->sge.length = wctx->len;
	wctx->sge.lkey = mr->lkey;
	wctx->wr.wwr.next = NULL;
	wctx->wr.wwr.wr_id = (uint64_t)wctx;
	wctx->wr.wwr.opcode = IBV_WR_SEND;
	wctx->wr.wwr.send_flags = IBV_SEND_SIGNALED;
	wctx->wr.wwr.sg_list = &wctx->sge;
	wctx->wr.wwr.num_sge = 1;

	ret = ibv_post_send(trans->qp, &wctx->wr.wwr, &trans->bad_send_wr);
	if (ret) {
		ERROR_LOG("ibv_post_send failed: %s (%d)", strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}


static void libercat_wait_callback(libercat_trans_t *trans, void *arg) {
	pthread_mutex_t *lock = arg;
	pthread_mutex_unlock(lock);
}

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * bad idea. do we want that one? Or place it on top of the queue? But sucks with asynchronism really
 */
int libercat_wait_recv(libercat_trans_t *trans, libercat_data_t **pdata, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&lock);
	libercat_post_recv(trans, pdata, mr, libercat_wait_callback, &lock);

	pthread_mutex_lock(&lock);
	pthread_mutex_unlock(&lock);
	pthread_mutex_destroy(&lock);
	return 0;
}

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans
 * @param data the size + opaque data.
 */
int libercat_wait_send(libercat_trans_t *trans, libercat_data_t *data, struct ibv_mr *mr) {
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&lock);
	libercat_post_send(trans, data, mr, libercat_wait_callback, &lock);

	pthread_mutex_lock(&lock);
	pthread_mutex_unlock(&lock);
	pthread_mutex_destroy(&lock);
	return 0;
}

// callbacks would all be run in a big send/recv_thread


// server specific:
int libercat_write(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size);
int libercat_read(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size);

// client specific:
int libercat_write_request(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size); // = ask for libercat_write server side ~= libercat_read
int libercat_read_request(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size); // = ask for libercat_read server side ~= libercat_write
