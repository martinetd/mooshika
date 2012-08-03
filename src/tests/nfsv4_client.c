/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet.ocre@cea.fr
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
 * \file   nfsv4_client.c
 * \brief  to be removed
 *
 * to be removed
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

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "mooshika.h"

#define CHUNK_SIZE 1024
#define RECV_NUM 3
#define NUM_SGE 4

#define TEST_Z(x)  do { if ( (x)) ERROR_LOG("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_NZ(x) do { if (!(x)) ERROR_LOG("error: " #x " failed (returned zero/null)."); } while (0)

struct datamr {
	void *data;
	struct ibv_mr *mr;
	msk_data_t *ackdata;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_send(msk_trans_t *trans, void *arg) {

}

void callback_disconnect(msk_trans_t *trans) {
}

void callback_recv(msk_trans_t *trans, void *arg) {
	struct datamr *datamr = arg;

	pthread_mutex_lock(datamr->lock);
	pthread_cond_signal(datamr->cond);
	pthread_mutex_unlock(datamr->lock);		
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
	attr.rq_depth = RECV_NUM+2;
	attr.addr.sa_in.sin_family = AF_INET;
	attr.addr.sa_in.sin_port = htons(1235);
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
				ERROR_LOG("verbose switch not ready just yet, come back later!\n");
				break;
			case 'c':
				attr.server = 0;
				inet_pton(AF_INET, optarg, &attr.addr.sa_in.sin_addr);
				break;
			case 's':
				attr.server = 10;
				inet_pton(AF_INET, "0.0.0.0", &attr.addr.sa_in.sin_addr);
				break;
			case 'S':
				attr.server = 10;
				inet_pton(AF_INET, optarg, &attr.addr.sa_in.sin_addr);
				break;
			case 'p':
				((struct sockaddr_in*) &attr.addr)->sin_port = htons(atoi(optarg));
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
		trans = msk_accept_one(trans);

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

	msk_data_t *rdata;
	struct datamr datamr;

	TEST_NZ(rdata = malloc(sizeof(msk_data_t)));
	rdata->data=rdmabuf; //+i*CHUNK_SIZE*sizeof(char);
	rdata->max_size=CHUNK_SIZE*sizeof(char);
	datamr.data = (void*)&(rdata);
	datamr.mr = mr;
	datamr.ackdata = ackdata; 
	datamr.lock = &lock;
	datamr.cond = &cond;

	pthread_mutex_lock(&lock);
	TEST_Z(msk_post_recv(trans, rdata, mr, callback_recv, &datamr));

	if (trans->server) {
		TEST_Z(msk_finalize_accept(trans));
	} else {
		TEST_Z(msk_finalize_connect(trans));
	}

	TEST_NZ(wdata = malloc(sizeof(msk_data_t)));
	wdata->data = rdmabuf+RECV_NUM*CHUNK_SIZE*sizeof(char);
	wdata->max_size = CHUNK_SIZE*sizeof(char);

	msk_rloc_t *rloc;

	if (trans->server) {
		printf("wait for rloc\n");
		TEST_Z(pthread_cond_wait(&cond, &lock)); // receive rloc

		TEST_NZ(rloc = malloc(sizeof(msk_rloc_t)));
		memcpy(rloc, rdata->data, sizeof(msk_rloc_t));
		printf("got rloc! key: %u, addr: %lu, size: %d\n", rloc->rkey, rloc->raddr, rloc->size);

		memcpy(wdata->data, "roses are red", 14);
		wdata->size = 14;

		TEST_Z(msk_post_write(trans, wdata, mr, rloc, callback_recv, &datamr));

		printf("waiting for write to finish\n");
		TEST_Z(pthread_cond_wait(&cond, &lock)); // write done

		TEST_Z(msk_post_recv(trans, rdata, mr, callback_recv, &datamr));
		TEST_Z(msk_post_send(trans, wdata, mr, NULL, NULL)); // ack to say we're done

		printf("waiting for something to be ready to read\n");
		TEST_Z(pthread_cond_wait(&cond, &lock));

		wdata->size=17;
		TEST_Z(msk_post_read(trans, wdata, mr, rloc, callback_recv, &datamr));

		printf("wait for read to finish\n");
		TEST_Z(pthread_cond_wait(&cond, &lock));

		printf("%s\n", wdata->data);

		TEST_Z(msk_wait_send(trans, wdata, mr)); // ack - other can quit


	} else {
		rloc = msk_make_rloc(mr, (uint64_t)ackdata->data, ackdata->max_size);

		memcpy(wdata->data, rloc, sizeof(msk_rloc_t));
		wdata->size = sizeof(msk_rloc_t);
		msk_post_send(trans, wdata, mr, NULL, NULL);

		printf("sent rloc, waiting for server to say they're done\n");
		TEST_Z(pthread_cond_wait(&cond, &lock)); // receive server ack (they wrote stuff)

		printf("%s\n", ackdata->data);

		TEST_Z(msk_post_recv(trans, rdata, mr, callback_recv, &datamr));

		memcpy(ackdata->data, "violets are blue", 17);
		TEST_Z(msk_post_send(trans, wdata, mr, NULL, NULL)); // say we've got something to read

		printf("waiting for server to be done\n");
		TEST_Z(pthread_cond_wait(&cond, &lock));

	}
	pthread_mutex_unlock(&lock);

	msk_destroy_trans(&trans);
	msk_destroy_trans(&trans); // check that double_destroy works

	return 0;
}
