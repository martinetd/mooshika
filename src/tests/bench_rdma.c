/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet@cea.fr
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
 * \file   bench_rdma.c
 * \brief  spams rdma read or write to check speed.
 *
 * spams rdma read or write to check speed.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <unistd.h>	//read
#include <getopt.h>
#include <errno.h>
#include <poll.h>

#include "utils.h"
#include "mooshika.h"

#define CHUNK_SIZE 1024*1024
#define SEND_COUNT 10000
#define RECV_NUM 2
#define msk_post_RW msk_post_read

struct datalock {
	msk_rloc_t *rloc;
	volatile int *count;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_send(msk_trans_t *trans, msk_data_t *data, void *arg) {

}

void callback_disconnect(msk_trans_t *trans) {
}

void callback_recv(msk_trans_t *trans, msk_data_t *data, void *arg) {
	struct datalock *datalock = arg;

	pthread_mutex_lock(datalock->lock);
	pthread_cond_signal(datalock->cond);
	pthread_mutex_unlock(datalock->lock);
}

void callback_read(msk_trans_t *trans, msk_data_t *data, void *arg) {
	struct datalock *datalock = arg;

	if (trans->state == MSK_CONNECTED && *datalock->count < SEND_COUNT)
		TEST_Z(msk_post_RW(trans, data, datalock->rloc, callback_read, NULL, datalock));

	*datalock->count += 1;
	pthread_cond_signal(datalock->cond);
}

void print_help(char **argv) {
	printf("Usage: %s {-s|-c addr}\n", argv[0]);
}

int main(int argc, char **argv) {


	msk_trans_t *trans;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;

	msk_data_t *wdata;

	msk_trans_attr_t attr;

	memset(&attr, 0, sizeof(msk_trans_attr_t));

	attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	attr.rq_depth = 1;
	attr.sq_depth = RECV_NUM+2; // RECV_NUM for read requets, one for the final wait_send, one to have a free one (post in a callback)
	attr.port = "1235";
//	attr.disconnect_callback = callback_disconnect;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "port",	required_argument,	0,		'p' },
		{ "help",	no_argument,		0,		'h' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op;
	while ((op = getopt_long(argc, argv, "@hvsS:c:p:", long_options, &option_index)) != -1) {
		switch(op) {
			case '@':
				printf("%s compiled on %s at %s\n", argv[0], __DATE__, __TIME__);
				printf("Release = %s\n", VERSION);
				printf("Release comment = %s\n", VERSION_COMMENT);
				printf("Git HEAD = %s\n", _GIT_HEAD_COMMIT ) ;
				printf("Git Describe = %s\n", _GIT_DESCRIBE ) ;
				exit(0);
			case 'h':
				print_help(argv);
				exit(0);
			case 'v':
				attr.debug = attr.debug * 2 + 1;
				break;
			case 'c':
				attr.server = 0;
				attr.node = optarg;
				break;
			case 's':
				attr.server = 10;
				attr.node = "::";
				break;
			case 'S':
				attr.server = 10;
				attr.node = optarg;
				break;
			case 'p':
				attr.port = optarg;
				break;
			default:
				ERROR_LOG("Failed to parse arguments");
				print_help(argv);
				exit(EINVAL);
		}
	}

	if (attr.server == -1) {
		ERROR_LOG("must be either a client or a server!");
		print_help(argv);
		exit(EINVAL);
	}

	TEST_Z(msk_init(&trans, &attr));

	if (!trans)
		exit(-1);


	if (trans->server) {
		TEST_Z(msk_bind_server(trans));
		TEST_NZ(trans = msk_accept_one(trans));
	} else { //client
		TEST_Z(msk_connect(trans));
	}

	TEST_NZ(rdmabuf = malloc((RECV_NUM+2)*CHUNK_SIZE*sizeof(char)));
	memset(rdmabuf, 0, (RECV_NUM+2)*CHUNK_SIZE*sizeof(char));
	TEST_NZ(mr = msk_reg_mr(trans, rdmabuf, (RECV_NUM+2)*CHUNK_SIZE*sizeof(char), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));



	msk_data_t *ackdata;
	TEST_NZ(ackdata = malloc(sizeof(msk_data_t)));
	ackdata->data = rdmabuf+(RECV_NUM+1)*CHUNK_SIZE*sizeof(char);
	ackdata->max_size = CHUNK_SIZE*sizeof(char);
	ackdata->size = 1;
	ackdata->data[0] = 0;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);

	msk_data_t **rdata;
	struct datalock *datalock;
	int i;

	TEST_NZ(rdata = malloc(RECV_NUM*sizeof(msk_data_t*)));
	TEST_NZ(datalock = malloc(RECV_NUM*sizeof(struct datalock)));

	for (i=0; i < RECV_NUM; i++) {
		TEST_NZ(rdata[i] = malloc(sizeof(msk_data_t)));
		rdata[i]->data=rdmabuf+i*CHUNK_SIZE*sizeof(char);
		rdata[i]->max_size=CHUNK_SIZE*sizeof(char);
		rdata[i]->mr = mr;
		datalock[i].lock = &lock;
		datalock[i].cond = &cond;
	}

	pthread_mutex_lock(&lock);
	TEST_Z(msk_post_recv(trans, rdata[0], callback_recv, NULL, &(datalock[0]))); // post only one, others will be used for reads

	if (trans->server) {
		TEST_Z(msk_finalize_accept(trans));
	} else {
		TEST_Z(msk_finalize_connect(trans));
	}

	TEST_NZ(wdata = malloc(sizeof(msk_data_t)));
	wdata->data = rdmabuf+RECV_NUM*CHUNK_SIZE*sizeof(char);
	wdata->max_size = CHUNK_SIZE*sizeof(char);
	wdata->mr = mr;

	msk_rloc_t *rloc;

	if (trans->server) {
		printf("wait for rloc\n");
		TEST_Z(pthread_cond_wait(&cond, &lock)); // receive rloc

		TEST_NZ(rloc = malloc(sizeof(msk_rloc_t)));
		memcpy(rloc, (rdata[0])->data, sizeof(msk_rloc_t));
		printf("got rloc! key: %u, addr: %lu, size: %d\n", rloc->rkey, rloc->raddr, rloc->size);

		volatile int count = 0;

		for (i=0; i < RECV_NUM; i++) {
			rdata[i]->size=CHUNK_SIZE*sizeof(char);
			datalock[i].rloc = rloc;
			datalock[i].count = &count;
			TEST_Z(msk_post_RW(trans, rdata[i], rloc, callback_read, NULL, &(datalock[i])));
		}

		while (count < SEND_COUNT) {
			pthread_cond_wait(&cond, &lock);
			if (count%100 == 0)
				printf("count: %d\n", count);
		}

		printf("count: %d\n", count);

		wdata->size = 1;
		TEST_Z(msk_post_send(trans, wdata, NULL, NULL, NULL)); // ack - other can quit
		usleep(10000); //FIXME: wait till last work request is done. cannot use wait_send because the other will get the send before we get our ack, so they might disconnect and our threads might fail before we get our WC that would unstuck us.

	} else {
		rloc = msk_make_rloc(mr, (uint64_t)ackdata->data, ackdata->max_size);

		memcpy(wdata->data, rloc, sizeof(msk_rloc_t));
		wdata->size = sizeof(msk_rloc_t);
		msk_post_send(trans, wdata, NULL, NULL, NULL);

		printf("sent rloc, waiting for server to say they're done\n");
		TEST_Z(pthread_cond_wait(&cond, &lock)); // receive server ack (they wrote stuff)

	}
	pthread_mutex_unlock(&lock);


	msk_destroy_trans(&trans);

	return 0;
}
