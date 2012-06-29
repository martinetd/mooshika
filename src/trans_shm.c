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
	int i;
	struct ibv_mr *mr;
	mr = malloc(sizeof(struct ibv_mr));
	if (!mr) {
		ERROR_LOG("malloc failed!");
		return NULL;
	}
	mr->addr = memaddr;
	mr->length = size;

	return mr;
}

/**
 * libercat_reg_mr: deregisters memory for rdma use (exactly ibv_dereg_mr)
 *
 * @param mr [INOUT] the mr to deregister
 *
 * @return 0 on success, errno value on failure
 */
int libercat_dereg_mr(struct ibv_mr *mr) {
	free(mr);
	return 0;
}

/**
 * libercat_make_rkey: makes a rkey to send it for remote host use
 * 
 * @param mr   [IN] the mr in which the addr belongs
 * @param addr [IN] the addr to give
 * @param size [IN] the size to allow (hint)
 *
 * @return a pointer to the rkey on success, NULL on failure.
 */
libercat_rloc_t *libercat_make_rkey(struct ibv_mr *mr, uint64_t addr, uint32_t size) {
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
 * libercat_destroy_trans: disconnects and free trans data
 *
 * @param trans [INOUT] the trans to destroy
 */
void libercat_destroy_trans(libercat_trans_t *trans) {

	pthread_mutex_destroy(&trans->lock);
	pthread_cond_destroy(&trans->cond);

	free(trans);
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

	trans->state = LIBERCAT_INIT;

	trans->server = attr->server;
	trans->timeout = attr->timeout ?: 3000000; // in ms
	trans->sq_depth = attr->sq_depth ?: 10;
	trans->rq_depth = attr->rq_depth ?: 50;

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

	return 0;
}



/**
 * libercat_accept: does the real connection acceptance
 *
 * @param trans [IN]
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_finalize_accept(libercat_trans_t *trans) {

	return 0;
}

/**
 * libercat_accept_one: given a listening trans, waits till one connection is requested and accepts it
 *
 * @param rdma_connection [IN] the mother trans
 *
 * @return a new trans for the child on success, NULL on failure
 */
int libercat_accept_one(libercat_trans_t *trans) {
	return 0;
}
/**
 * libercat_connect_client: does the actual connection to the server
 *
 */
int libercat_finalize_connect(libercat_trans_t *trans) {
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

	return 0;
}



/**
 * libercat_post_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans        [IN]
 * @param pdata        [OUT] the data buffer to be filled with received data
 * @param mr           [IN]  the mr in which the data lives
 * @param callback     [IN]  function that'll be called when done
 * @param callback_arg [IN]  argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
int libercat_post_recv(libercat_trans_t *trans, libercat_data_t **pdata, struct ibv_mr *mr, ctx_callback_t callback, void* callback_arg) {
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

	return 0;
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
int libercat_write(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size);
int libercat_read(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size);

// client specific:
int libercat_write_request(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size); // = ask for libercat_write server side ~= libercat_read
int libercat_read_request(libercat_trans_t *trans, libercat_rloc_t *libercat_rloc, size_t size); // = ask for libercat_read server side ~= libercat_write
