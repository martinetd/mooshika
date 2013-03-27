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
 * \file   rcat.c
 * \brief  Example of usage/most basic test program for mooshika
 *
 * Example of usage/most basic test program for mooshika
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

#include "log.h"
#include "mooshika.h"

#define CHUNK_SIZE 8*1024
#define RECV_NUM 1

#define TEST_Z(x)  do { if ( (x)) { ERROR_LOG("error: " #x " failed (returned non-zero)." ); exit(-1); }} while (0)
#define TEST_NZ(x) do { if (!(x)) { ERROR_LOG("error: " #x " failed (returned zero/null)."); exit(-1); }} while (0)

struct datamr {
	msk_data_t *data;
	struct ibv_mr *mr;
	msk_data_t *ackdata;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_send(msk_trans_t *trans, void *arg) {

}

void callback_disconnect(msk_trans_t *trans) {
	if (!trans->private_data)
		return;

	struct datamr *datamr = trans->private_data;
	pthread_mutex_lock(datamr->lock);
	pthread_cond_signal(datamr->cond);
	pthread_mutex_unlock(datamr->lock);
}

void callback_recv(msk_trans_t *trans, void *arg) {
	struct datamr *datamr = arg;
	int n;

	if (!datamr) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	msk_data_t *pdata = datamr->data;

	if (pdata->size != 1 || pdata->data[0] != '\0') {
	// either we get real data and write it to stdout
		n = write(1, (char *)pdata->data, pdata->size);
		fflush(stdout);

		if (n != pdata->size)
			ERROR_LOG("Wrote less than what was actually received");

		msk_post_recv(trans, pdata, datamr->mr, callback_recv, datamr);
		msk_post_send(trans, datamr->ackdata, datamr->mr, NULL, NULL);
	} else {
	// or we get an ack and just send a signal to handle_thread thread
		msk_post_recv(trans, pdata, datamr->mr, callback_recv, datamr);

		pthread_mutex_lock(datamr->lock);
		pthread_cond_signal(datamr->cond);
		pthread_mutex_unlock(datamr->lock);
	}
}

void print_help(char **argv) {
	printf("Usage: %s {-s|-c addr}\n", argv[0]);
}

void* handle_trans(void *arg) {
	msk_trans_t *trans = arg;
	int mt_server = *(int*)trans->private_data;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;
	msk_data_t *wdata;
	msk_data_t *ackdata;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	msk_data_t **rdata;
	struct datamr *datamr;
	int i;

	struct pollfd pollfd_stdin;


	// malloc memory zone that will contain all buffer data (for mr), and register it for our trans
#define RDMABUF_SIZE (RECV_NUM+2)*CHUNK_SIZE
	TEST_NZ(rdmabuf = malloc(RDMABUF_SIZE));
	memset(rdmabuf, 0, RDMABUF_SIZE);
	TEST_NZ(mr = msk_reg_mr(trans, rdmabuf, RDMABUF_SIZE, IBV_ACCESS_LOCAL_WRITE));


	// malloc mooshika's data structs (i.e. max_size+size+pointer to actual data), for ack buffer
	TEST_NZ(ackdata = malloc(sizeof(msk_data_t)));
	ackdata->data = rdmabuf+(RECV_NUM+1)*CHUNK_SIZE;
	ackdata->max_size = CHUNK_SIZE;
	ackdata->size = 1;
	ackdata->data[0] = 0;

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);

	// malloc receive structs as well as a custom callback argument, and post it for future receive
	TEST_NZ(rdata = malloc(RECV_NUM*sizeof(msk_data_t*)));
	TEST_NZ(datamr = malloc(RECV_NUM*sizeof(struct datamr)));

	for (i=0; i < RECV_NUM; i++) {
		TEST_NZ(rdata[i] = malloc(sizeof(msk_data_t)));
		rdata[i]->data=rdmabuf+i*CHUNK_SIZE;
		rdata[i]->max_size=CHUNK_SIZE;
		datamr[i].data = rdata[i];
		datamr[i].mr = mr;
		datamr[i].ackdata = ackdata; 
		datamr[i].lock = &lock;
		datamr[i].cond = &cond;
		TEST_Z(msk_post_recv(trans, rdata[i], mr, callback_recv, &(datamr[i])));
	}

	trans->private_data = datamr;

	// receive buffers are posted, we can finalize the connection
	if (trans->server) {
		TEST_Z(msk_finalize_accept(trans));
	} else {
		TEST_Z(msk_finalize_connect(trans));
	}

	// malloc write (send) structs to post data read from stdin
	TEST_NZ(wdata = malloc(sizeof(msk_data_t)));
	wdata->data = rdmabuf+RECV_NUM*CHUNK_SIZE;
	wdata->max_size = CHUNK_SIZE;

	pollfd_stdin.fd = 0; // stdin
	pollfd_stdin.events = POLLIN | POLLPRI;
	pollfd_stdin.revents = 0;

	while (trans->state == MSK_CONNECTED) {

		i = poll(&pollfd_stdin, 1, 100);

		if (i == -1)
			break;

		if (i == 0)
			continue;

		wdata->size = read(0, (char*)wdata->data, wdata->max_size);
		if (wdata->size == 0)
			break;

		// post our data and wait for the other end's ack (sent in callback_recv)
		pthread_mutex_lock(&lock);
		TEST_Z(msk_post_send(trans, wdata, mr, NULL, NULL));
		pthread_cond_wait(&cond, &lock);
		pthread_mutex_unlock(&lock);
	}	


	msk_destroy_trans(&trans);

	// free stuff
	free(wdata);
	free(datamr);
	free(rdata);
	free(ackdata);
	free(rdmabuf);

	if (mt_server)
		pthread_exit(NULL);
	else
		return NULL;
}

int main(int argc, char **argv) {

	msk_trans_t *trans;
	msk_trans_t *child_trans;

	msk_trans_attr_t attr;

	int mt_server = 0;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "port",	required_argument,	0,		'p' },
		{ "help",	no_argument,		0,		'h' },
		{ "multi",	no_argument,		0,		'm' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op;

	memset(&attr, 0, sizeof(msk_trans_attr_t));

	attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	attr.rq_depth = RECV_NUM+2;
	attr.addr.sa_in.sin_family = AF_INET;
	attr.addr.sa_in.sin_port = htons(1235);
	attr.disconnect_callback = callback_disconnect;

	while ((op = getopt_long(argc, argv, "@hvmsS:c:p:", long_options, &option_index)) != -1) {
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
			case 'm':
				mt_server = 1;
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


	trans->private_data = &mt_server;

	if (trans->server) {
		pthread_t id;
		pthread_attr_t attr_thr;

		TEST_Z(msk_bind_server(trans));

		/* Init for thread parameter (mostly for scheduling) */
		if(pthread_attr_init(&attr_thr) != 0)
			ERROR_LOG("can't init pthread's attributes");

		if(pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM) != 0)
			ERROR_LOG("can't set pthread's scope");

		if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE) != 0)
			ERROR_LOG("can't set pthread's join state");

		if (mt_server) {
			while (1) {
				child_trans = msk_accept_one(trans);
				if (!child_trans) {
					ERROR_LOG("accept_one failed!");
					break;
				}
				pthread_create(&id, &attr_thr, handle_trans, child_trans);
			}
		} else {
			child_trans = msk_accept_one(trans);
			handle_trans(child_trans);
		}
		msk_destroy_trans(&trans);
	} else { //client
		TEST_Z(msk_connect(trans));
		handle_trans(trans);
	       
	}

	return 0;
}
