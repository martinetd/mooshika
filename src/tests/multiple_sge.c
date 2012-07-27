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
 * \file   multiple_sge.c
 * \brief  tests operations with multiple send/gather elements
 *
 * tests operations with multiple send/gather elements
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

#define CHUNK_SIZE 10
#define RECV_NUM 1
#define NUM_SGE 2

#define TEST_Z(x)  do { if ( (x)) ERROR_LOG("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_NZ(x) do { if (!(x)) ERROR_LOG("error: " #x " failed (returned zero/null)."); } while (0)

struct datalock {
	struct ibv_mr *mr;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_send(msk_trans_t *trans, void *arg) {

}

void callback_disconnect(msk_trans_t *trans) {
}

void callback_recv(msk_trans_t *trans, void *arg) {
	struct datalock *datalock = arg;

	pthread_mutex_lock(datalock->lock);
	pthread_cond_signal(datalock->cond);
	pthread_mutex_unlock(datalock->lock);		
}


void print_help(char **argv) {
	printf("Usage: %s {-s|-c addr}\n", argv[0]);
}

int main(int argc, char **argv) {


	msk_trans_t *trans;
	uint8_t *mrbuf;
	struct ibv_mr *mr;

	msk_data_t *wdata;

	msk_trans_attr_t attr;

	memset(&attr, 0, sizeof(msk_trans_attr_t));

	attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	attr.rq_depth = RECV_NUM+2;
	attr.sq_depth = RECV_NUM+2;
	attr.max_recv_sge = NUM_SGE;
	attr.max_send_sge = NUM_SGE;
	attr.addr.sa_in.sin_family = AF_INET;
	attr.addr.sa_in.sin_port = htons(1235);
//	attr.disconnect_callback = callback_disconnect;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "port",	required_argument,	0,		'p' },
		{ "server",	no_argument,		0,		's' },
		{ "help",	no_argument,		0,		'h' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op;
	while ((op = getopt_long(argc, argv, "@hvsS:c:p:", long_options, &option_index)) != -1) { /*  */
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

		TEST_Z(msk_start_cm_thread(trans));
	} else { //client
		TEST_Z(msk_connect(trans));
	}


	TEST_NZ(mrbuf = malloc((RECV_NUM*NUM_SGE+1)*CHUNK_SIZE));
	memset(mrbuf, 0, (RECV_NUM*NUM_SGE+1)*CHUNK_SIZE);
	TEST_NZ(mr = msk_reg_mr(trans, mrbuf, (RECV_NUM*NUM_SGE+1)*CHUNK_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));



	pthread_mutex_t lock;
	pthread_cond_t cond;

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);

	msk_data_t *rdata;
	struct datalock datalock;

	TEST_NZ(rdata = malloc(RECV_NUM*NUM_SGE*sizeof(msk_data_t)));
	int i;
	for (i=0; i<RECV_NUM*NUM_SGE; i++) {
		rdata[i].data=mrbuf+i*CHUNK_SIZE;
		rdata[i].size = 0;
		rdata[i].max_size=CHUNK_SIZE;
	}
	datalock.mr = mr;
	datalock.lock = &lock;
	datalock.cond = &cond;

	TEST_NZ(wdata = malloc(sizeof(msk_data_t)));
	wdata->data = mrbuf+RECV_NUM*NUM_SGE*CHUNK_SIZE;
	wdata->max_size = CHUNK_SIZE;


	pthread_mutex_lock(&lock);
	if (trans->server) // server receives, client sends
		TEST_Z(msk_post_n_recv(trans, rdata, NUM_SGE, mr, callback_recv, &datalock));


	if (trans->server) {
		TEST_Z(msk_finalize_accept(trans));
	} else {
		TEST_Z(msk_finalize_connect(trans));
	}



	if (trans->server) {
		TEST_Z(pthread_cond_wait(&cond, &lock));

		printf("Got something:\n %s (%d), %s (%d)\n", rdata[0].data, rdata[0].size, rdata[1].data, rdata[1].size);

	} else {

		memcpy(rdata[0].data, "012345678", 10);
		rdata[0].size = 10;
		memcpy(rdata[1].data, "0123456", 8);
		rdata[1].size = 8;

		TEST_Z(msk_post_n_send(trans, rdata, NUM_SGE, mr, callback_recv, &datalock));

		TEST_Z(pthread_cond_wait(&cond, &lock));

		printf("Done with send\n");
	}

	pthread_mutex_unlock(&lock);

	msk_destroy_trans(&trans);

	sleep(1);

	return 0;
}
