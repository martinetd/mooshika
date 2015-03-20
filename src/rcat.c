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
#include <inttypes.h> // PRIu64


#include "utils.h"
#include "mooshika.h"

#define DEFAULT_BLOCK_SIZE 1024*1024
#define DEFAULT_RECV_NUM 4

struct priv_data {
	msk_data_t *ackdata;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
	int ack;
};

struct thread_arg {
	int mt_server;
	int stats;
	int recv_num;
	uint32_t block_size;
	msk_data_t *rdata;
};

void callback_send(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
}

void callback_disconnect(msk_trans_t *trans) {
	if (!trans->private_data)
		return;

	struct priv_data *priv_data = trans->private_data;
	pthread_mutex_lock(priv_data->lock);
	pthread_cond_signal(priv_data->cond);
	pthread_mutex_unlock(priv_data->lock);
}


void callback_error(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	INFO_LOG(trans->state != MSK_CLOSING && trans->state != MSK_CLOSED
		&& trans->debug, "error callback on buffer %p", pdata);
}

void callback_recv(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	struct priv_data *priv_data = trans->private_data;
	int n;

	if (!priv_data) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	if (pdata->size != 1 || pdata->data[0] != '\0') {
	// either we get real data and write it to stdout/send ack
		n = write(1, (char *)pdata->data, pdata->size);
		fflush(stdout);

		if (n != pdata->size)
			ERROR_LOG("Wrote less than what was actually received");

		if (msk_post_recv(trans, pdata, callback_recv, callback_error, priv_data))
			ERROR_LOG("post_recv failed");
	        if (msk_post_send(trans, priv_data->ackdata, NULL, NULL, NULL))
			ERROR_LOG("post_send failed");
	} else {
	// or we get an ack and just send a signal to handle_trans thread
		if(msk_post_recv(trans, pdata, callback_recv, callback_error, priv_data))
			ERROR_LOG("post_recv failed");

		pthread_mutex_lock(priv_data->lock);
		priv_data->ack = 0;
		pthread_cond_signal(priv_data->cond);
		pthread_mutex_unlock(priv_data->lock);
	}
}

void print_help(char **argv) {
	printf("Usage: %s {-s|-c addr} [-p port] [-m] [-v] [-b blocksize] [-r recvnum]\n", argv[0]);
	printf("Mandatory argument, either of:\n"
		"	-c, --client addr: client to connect to\n"
		"	-s, --server: server mode\n"
		"	-S addr: server mode, bind to address\n"
		"Optional arguments:\n"
		"	-p, --port port: port to use\n"
		"	-m, --multi: server only, multithread/accept multiple connections\n"
		"	-v, --verbose: enable verbose output (more v for more verbosity)\n"
		"	-q, --quiet: don't display connection messages\n"
		"	-D, --stats <prefix>: create a socket where to look stats up at given path\n"
		"	-d: display stats summary on close\n"
		"	-b, --block-size size: size of packets to send (default: %u)\n"
		"	-r, --recv-num n: size of receive queue for server (default: %u)\n",
		DEFAULT_BLOCK_SIZE, DEFAULT_RECV_NUM);
}

void* handle_trans(void *arg) {
	msk_trans_t *trans = arg;
	struct thread_arg *thread_arg = trans->private_data;
	struct ibv_mr *mr;
	msk_data_t *ackdata;
	msk_data_t *wdatas;
	int cur_data = 0;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	struct priv_data *priv_data;
	int i;

	struct pollfd pollfd_stdin;


	// malloc mooshika's data structs (i.e. max_size+size+pointer to actual data), for ack buffer
	TEST_NZ(ackdata = malloc(sizeof(msk_data_t)+1));
	TEST_NZ(mr = msk_reg_mr(trans, (uint8_t*)(ackdata + 1), 1, IBV_ACCESS_LOCAL_WRITE));
	ackdata->data = (uint8_t*)(ackdata + 1);
	ackdata->max_size = 1;
	ackdata->size = 1;
	ackdata->data[0] = 0;
	ackdata->mr = mr;

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);

	TEST_NZ(priv_data = malloc(sizeof(struct priv_data)));

	priv_data->ackdata = ackdata;
	priv_data->lock    = &lock;
	priv_data->cond    = &cond;
	priv_data->ack     = 0;

	trans->private_data = priv_data;

	// receive buffers are posted, we can finalize the connection
	if (trans->server) {
		TEST_Z(msk_finalize_accept(trans));
	} else {
		TEST_Z(msk_finalize_connect(trans));
	}


	// malloc write (send) structs to post data read from stdin
	TEST_NZ(wdatas = malloc(2*(sizeof(msk_data_t)+thread_arg->block_size)));
	TEST_NZ(mr = msk_reg_mr(trans, (uint8_t*)(wdatas+2), 2*thread_arg->block_size, IBV_ACCESS_LOCAL_WRITE));
	wdatas[0].data = (uint8_t*)(wdatas+2);
	wdatas[1].data = wdatas[0].data + thread_arg->block_size;
	wdatas[0].max_size = thread_arg->block_size;
	wdatas[1].max_size = thread_arg->block_size;
	wdatas[0].mr = mr;
	wdatas[1].mr = mr;


	pollfd_stdin.fd = 0; // stdin
	pollfd_stdin.events = POLLIN | POLLPRI;
	pollfd_stdin.revents = 0;

	while (trans->state == MSK_CONNECTED) {

		i = poll(&pollfd_stdin, 1, 100);

		if (i == -1)
			break;

		if (i == 0)
			continue;

		wdatas[cur_data].size = read(0, (char*)wdatas[cur_data].data, wdatas[cur_data].max_size);
		if (wdatas[cur_data].size == 0)
			break;


		// Make sure we're done waiting and declare we're waiting for next
		pthread_mutex_lock(&lock);
		while (trans->state == MSK_CONNECTED && priv_data->ack) {
			pthread_cond_wait(&cond, &lock);
		}
		priv_data->ack = 1;
		pthread_mutex_unlock(&lock);

		// can fail if e.g. other side already has hung up
		// (can explain error callbacks too, e.g. post_send ok, hang up, actual send fails)
		if (msk_post_send(trans, wdatas + cur_data, NULL, NULL, NULL))
			break;

		cur_data = (cur_data + 1) % 2;
	}	

	pthread_mutex_lock(&lock);
	if (trans->state == MSK_CONNECTED && priv_data->ack) {
		// We're the ones closing, least we can do is wrap it up
		pthread_cond_wait(&cond, &lock);
	}
	pthread_mutex_unlock(&lock);

	if (thread_arg->stats)
		fprintf(stderr,
			"stats:\n"
			"	tx_bytes\ttx_pkt\ttx_err\n"
			"	%10"PRIu64"\t%"PRIu64"\t%"PRIu64"\n"
			"	rx_bytes\trx_pkt\trx_err\n"
			"	%10"PRIu64"\t%"PRIu64"\t%"PRIu64"\n"
			"	callback time:   %lu.%09lu s\n"
			"	completion time: %lu.%09lu s\n",
			trans->stats.tx_bytes, trans->stats.tx_pkt,
			trans->stats.tx_err, trans->stats.rx_bytes,
			trans->stats.rx_pkt, trans->stats.rx_err,
			trans->stats.nsec_callback / NSEC_IN_SEC, trans->stats.nsec_callback % NSEC_IN_SEC,
			trans->stats.nsec_compevent / NSEC_IN_SEC, trans->stats.nsec_compevent % NSEC_IN_SEC);


	TEST_Z(msk_dereg_mr(wdatas->mr));
	TEST_Z(msk_dereg_mr(ackdata->mr));

	msk_destroy_trans(&trans);

	// free stuff
	free(wdatas);
	free(priv_data);
	free(ackdata);

	if (thread_arg->mt_server)
		pthread_exit(NULL);
	else
		return NULL;
}


void post_recvs(msk_trans_t *trans, struct thread_arg *thread_arg) {
	uint8_t	*rdmabuf;
	struct ibv_mr *mr;
	int i;

	// malloc memory zone that will contain all buffer data (for mr), and register it for our trans
#define RDMABUF_SIZE (thread_arg->recv_num+2)*thread_arg->block_size
	TEST_NZ(rdmabuf = malloc(RDMABUF_SIZE));
	memset(rdmabuf, 0, RDMABUF_SIZE);
	TEST_NZ(mr = msk_reg_mr(trans, rdmabuf, RDMABUF_SIZE, IBV_ACCESS_LOCAL_WRITE));
	// malloc receive structs as well as a custom callback argument, and post it for future receive
	TEST_NZ(thread_arg->rdata = malloc(thread_arg->recv_num*sizeof(msk_data_t)));
	for (i=0; i < thread_arg->recv_num; i++) {
		thread_arg->rdata[i].data=rdmabuf+i*thread_arg->block_size;
		thread_arg->rdata[i].max_size=thread_arg->block_size;
		thread_arg->rdata[i].mr = mr;
		TEST_Z(msk_post_recv(trans, &thread_arg->rdata[i], callback_recv, callback_error, NULL));
	}
}

int setup_recv(msk_trans_t *trans, struct thread_arg *thread_arg) {
	struct msk_pd *pd;


	if (trans->srq) {
		TEST_NZ(pd = msk_getpd(trans));
		if (!pd->private) {
			post_recvs(trans, thread_arg);
			pd->private = (void*)1;
		}
	} else {
		post_recvs(trans, thread_arg);
	}

	return 0;
}

int main(int argc, char **argv) {

	msk_trans_t *trans;
	msk_trans_t *child_trans;

	msk_trans_attr_t attr;

	struct thread_arg thread_arg;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	no_argument,		0,		's' },
		{ "port",	required_argument,	0,		'p' },
		{ "help",	no_argument,		0,		'h' },
		{ "multi",	no_argument,		0,		'm' },
		{ "verbose",	no_argument,		0,		'v' },
		{ "quiet",	no_argument,		0,		'q' },
		{ "stats",	required_argument,	0,		'D' },
		{ "recv-num",	required_argument,	0,		'r' },
		{ "block-size",	required_argument,	0,		'b' },
		{ "srq",	no_argument,		0,		'x' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op;
	char *tmp_s;

	memset(&attr, 0, sizeof(msk_trans_attr_t));
	memset(&thread_arg, 0, sizeof(struct thread_arg));

	attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	attr.debug = 1;
	attr.use_srq = 0;
	attr.disconnect_callback = callback_disconnect;
	attr.port = "1235"; /* default port */

	while ((op = getopt_long(argc, argv, "@hvqmsb:S:c:p:dD:r:x", long_options, &option_index)) != -1) {
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
					set_size(thread_arg.block_size, tmp_s);
				}
				INFO_LOG(attr.debug > 1, "block size: %u", thread_arg.block_size);
				break;
			case 'r':
				thread_arg.recv_num = strtoul(optarg, &tmp_s, 0);
				if (errno || thread_arg.recv_num == 0) {
					thread_arg.recv_num = 0;
					ERROR_LOG("Invalid block size, assuming default (%u)", DEFAULT_RECV_NUM);
				}
				break;
			case 'h':
				print_help(argv);
				exit(0);
			case 'v':
				attr.debug = attr.debug * 2 + 1;
				break;
			case 'd':
				thread_arg.stats = 1;
				break;
			case 'D':
				attr.stats_prefix = optarg;
				thread_arg.stats = 1;
				break;
			case 'q':
				attr.debug = 0;
				break;
			case 'c':
				attr.server = 0;
				attr.node = optarg;
				break;
			case 's':
				attr.server = 64;
				attr.node = "::";
				break;
			case 'S':
				attr.server = 64;
				attr.node = optarg;
				break;
			case 'p':
				attr.port = optarg;
				break;
			case 'm':
				thread_arg.mt_server = 1;
				break;
			case 'x':
				attr.use_srq = 1;
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
	if (thread_arg.stats)
		attr.debug |= MSK_DEBUG_SPEED;
	if (thread_arg.recv_num == 0)
		thread_arg.recv_num = attr.use_srq ? DEFAULT_RECV_NUM : 2;

	attr.rq_depth = thread_arg.recv_num+2;

	// writing to stdout is the limiting factor anyway
	attr.worker_count = -1;

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

		if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_DETACHED) != 0)
			ERROR_LOG("can't set pthread's join state");

		if (thread_arg.mt_server) {
			while (1) {
				child_trans = msk_accept_one(trans);
				if (!child_trans) {
					ERROR_LOG("accept_one failed!");
					break;
				}
				TEST_Z(setup_recv(child_trans, &thread_arg));
				pthread_create(&id, &attr_thr, handle_trans, child_trans);
			}
		} else {
			TEST_NZ(child_trans = msk_accept_one(trans));
			TEST_Z(setup_recv(child_trans, &thread_arg));
			handle_trans(child_trans);
		}
		msk_destroy_trans(&trans);
	} else { //client
		TEST_Z(msk_connect(trans));
		TEST_Z(setup_recv(trans, &thread_arg));
		handle_trans(trans);
	}


	free(thread_arg.rdata);

	return 0;
}
