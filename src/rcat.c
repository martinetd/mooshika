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
#include <netdb.h>      //gethostbyname
#include <getopt.h>
#include <errno.h>
#include <poll.h>

#include "utils.h"
#include "mooshika.h"

#define DEFAULT_BLOCK_SIZE 1024*1024
#define RECV_NUM 1

#define TEST_Z(x)  do { if ( (x)) { ERROR_LOG("error: " #x " failed (returned non-zero)." ); }} while (0)
#define TEST_NZ(x) do { if (!(x)) { ERROR_LOG("error: " #x " failed (returned zero/null)."); }} while (0)

struct cb_arg {
	struct ibv_mr *mr;
	msk_data_t *ackdata;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

struct thread_arg {
	int mt_server;
	uint32_t block_size;
};

void callback_send(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
}

void callback_disconnect(msk_trans_t *trans) {
	if (!trans->private_data)
		return;

	struct cb_arg *cb_arg = trans->private_data;
	pthread_mutex_lock(cb_arg->lock);
	pthread_cond_signal(cb_arg->cond);
	pthread_mutex_unlock(cb_arg->lock);
}


void callback_error(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
}

void callback_recv(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	struct cb_arg *cb_arg = arg;
	int n;

	if (!cb_arg) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	if (pdata->size != 1 || pdata->data[0] != '\0') {
	// either we get real data and write it to stdout/send ack
		n = write(1, (char *)pdata->data, pdata->size);
		fflush(stdout);

		if (n != pdata->size)
			ERROR_LOG("Wrote less than what was actually received");

		TEST_Z(msk_post_recv(trans, pdata, cb_arg->mr, callback_recv, callback_error, cb_arg));
		TEST_Z(msk_post_send(trans, cb_arg->ackdata, cb_arg->mr, NULL, NULL, NULL));
	} else {
	// or we get an ack and just send a signal to handle_thread thread
		TEST_Z(msk_post_recv(trans, pdata, cb_arg->mr, callback_recv, callback_error, cb_arg));

		pthread_mutex_lock(cb_arg->lock);
		pthread_cond_signal(cb_arg->cond);
		pthread_mutex_unlock(cb_arg->lock);
	}
}

void print_help(char **argv) {
	printf("Usage: %s {-s|-c addr} [-p port] [-m] [-v] [-b blocksize]\n", argv[0]);
	printf("Mandatory argument, either of:\n"
		"	-c, --client addr: client to connect to\n"
		"	-s, --server: server mode\n"
		"	-S addr: server mode, bind to address\n"
		"Optional arguments:\n"
		"	-p, --port port: port to use\n"
		"	-m, --multi: server only, multithread/accept multiple connections\n"
		"	-v, --verbose: enable verbose output\n"
		"	-b, --block-size size: size of packets to send (default: %u)\n", DEFAULT_BLOCK_SIZE);
}

void* handle_trans(void *arg) {
	msk_trans_t *trans = arg;
	struct thread_arg *thread_arg = trans->private_data;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;
	msk_data_t *wdata;
	msk_data_t *ackdata;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	msk_data_t **rdata;
	struct cb_arg *cb_arg;
	int i;

	struct pollfd pollfd_stdin;


	// malloc memory zone that will contain all buffer data (for mr), and register it for our trans
#define RDMABUF_SIZE (RECV_NUM+2)*thread_arg->block_size
	TEST_NZ(rdmabuf = malloc(RDMABUF_SIZE));
	memset(rdmabuf, 0, RDMABUF_SIZE);
	TEST_NZ(mr = msk_reg_mr(trans, rdmabuf, RDMABUF_SIZE, IBV_ACCESS_LOCAL_WRITE));


	// malloc mooshika's data structs (i.e. max_size+size+pointer to actual data), for ack buffer
	TEST_NZ(ackdata = malloc(sizeof(msk_data_t)));
	ackdata->data = rdmabuf+(RECV_NUM+1)*thread_arg->block_size;
	ackdata->max_size = thread_arg->block_size;
	ackdata->size = 1;
	ackdata->data[0] = 0;

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);

	// malloc receive structs as well as a custom callback argument, and post it for future receive
	TEST_NZ(rdata = malloc(RECV_NUM*sizeof(msk_data_t*)));
	TEST_NZ(cb_arg = malloc(RECV_NUM*sizeof(struct cb_arg)));

	for (i=0; i < RECV_NUM; i++) {
		TEST_NZ(rdata[i] = malloc(sizeof(msk_data_t)));
		rdata[i]->data=rdmabuf+i*thread_arg->block_size;
		rdata[i]->max_size=thread_arg->block_size;
		cb_arg[i].mr = mr;
		cb_arg[i].ackdata = ackdata;
		cb_arg[i].lock = &lock;
		cb_arg[i].cond = &cond;
		TEST_Z(msk_post_recv(trans, rdata[i], mr, callback_recv, callback_error, &(cb_arg[i])));
	}

	trans->private_data = cb_arg;

	// receive buffers are posted, we can finalize the connection
	if (trans->server) {
		TEST_Z(msk_finalize_accept(trans));
	} else {
		TEST_Z(msk_finalize_connect(trans));
	}


	// malloc write (send) structs to post data read from stdin
	TEST_NZ(wdata = malloc(sizeof(msk_data_t)));
	wdata->data = rdmabuf+RECV_NUM*thread_arg->block_size;
	wdata->max_size = thread_arg->block_size;

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
		TEST_Z(msk_post_send(trans, wdata, mr, NULL, NULL, NULL));
		pthread_cond_wait(&cond, &lock);
		pthread_mutex_unlock(&lock);
	}	


	msk_destroy_trans(&trans);

	// free stuff
	free(wdata);
	free(cb_arg);
	free(rdata);
	free(ackdata);
	free(rdmabuf);

	if (thread_arg->mt_server)
		pthread_exit(NULL);
	else
		return NULL;
}

int main(int argc, char **argv) {

	msk_trans_t *trans;
	msk_trans_t *child_trans;

	msk_trans_attr_t attr;

	struct thread_arg thread_arg;

	struct hostent *host;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	no_argument,		0,		's' },
		{ "port",	required_argument,	0,		'p' },
		{ "help",	no_argument,		0,		'h' },
		{ "multi",	no_argument,		0,		'm' },
		{ "verbose",	no_argument,		0,		'v' },
		{ "block-size",	required_argument,	0,		'b' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op;
	char *tmp_s;

	memset(&attr, 0, sizeof(msk_trans_attr_t));
	memset(&thread_arg, 0, sizeof(struct thread_arg));

	attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	attr.rq_depth = RECV_NUM+2;
	attr.addr.sa_in.sin_family = AF_INET;
	attr.addr.sa_in.sin_port = htons(1235);
	attr.disconnect_callback = callback_disconnect;

	while ((op = getopt_long(argc, argv, "@hvmsb:S:c:p:", long_options, &option_index)) != -1) {
		switch(op) {
			case '@':
				printf("%s compiled on %s at %s\n", argv[0], __DATE__, __TIME__);
				printf("Release = %s\n", VERSION);
				printf("Release comment = %s\n", VERSION_COMMENT);
				printf("Git HEAD = %s\n", _GIT_HEAD_COMMIT ) ;
				printf("Git Describe = %s\n", _GIT_DESCRIBE ) ;
				exit(0);
			case 'b':
				thread_arg.block_size = strtoul(optarg, &tmp_s, 0);
				if (errno || thread_arg.block_size == 0) {
					thread_arg.block_size = 0;
					ERROR_LOG("Invalid block size, assuming default (%u)", DEFAULT_BLOCK_SIZE);
					break;
				}
				if (tmp_s[0] != 0) {
					set_size(&thread_arg.block_size, tmp_s);
				}
				INFO_LOG(attr.debug, "block size: %u", thread_arg.block_size);
				break;
			case 'h':
				print_help(argv);
				exit(0);
			case 'v':
				attr.debug = 1;
				break;
			case 'c':
				attr.server = 0;
				host = gethostbyname(optarg);
				//FIXME: if (host->h_addrtype == AF_INET6) {
				memcpy(&attr.addr.sa_in.sin_addr, host->h_addr_list[0], 4);
				break;
			case 's':
				attr.server = 10;
				inet_pton(AF_INET, "0.0.0.0", &attr.addr.sa_in.sin_addr);
				break;
			case 'S':
				attr.server = 10;
				host = gethostbyname(optarg);
				//FIXME: if (host->h_addrtype == AF_INET6) {
				memcpy(&attr.addr.sa_in.sin_addr, host->h_addr_list[0], 4);
				break;
			case 'p':
				((struct sockaddr_in*) &attr.addr)->sin_port = htons(atoi(optarg));
				break;
			case 'm':
				thread_arg.mt_server = 1;
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

	if (thread_arg.block_size == 0)
		thread_arg.block_size = DEFAULT_BLOCK_SIZE;

	// writing to stdout is the limiting factor anyway
	attr.worker_count = -1;
	attr.worker_queue_size = 100;

	TEST_Z(msk_init(&trans, &attr));

	if (!trans)
		exit(-1);


	trans->private_data = &thread_arg;

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

		if (thread_arg.mt_server) {
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
