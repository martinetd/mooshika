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
 * \file   rmitm.c
 * \brief  Example of usage/man in the middle for rdma send
 *
 * Example of usage/man in the middle for rdma send
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
#include <sys/types.h>	//open
#include <sys/stat.h>	//open
#include <fcntl.h>	//open
#include <signal.h>
#include <inttypes.h> // PRIu64

#include <pcap/pcap.h>
#include <linux/if_arp.h>

#include "utils.h"
#include "mooshika.h"
#include "rmitm.h"

#define DEFAULT_BLOCK_SIZE 1024*1024 // nfs page size
#define DEFAULT_RECV_NUM 16
#define DEFAULT_HARD_TRUNC_LEN (64*1024-1)
#define DEFAULT_TRUNC_LEN 4096


struct datalock {
	msk_data_t *data;
	pthread_mutex_t lock;
};

struct privatedata {
	uint32_t seq_nr;
	msk_trans_t *o_trans;
	struct datalock *first_datalock;
	struct thread_arg *targ;
};

struct thread_arg {
	pcap_dumper_t **p_pcap_dumper;
	char *pcap_filename;
	pcap_t *pcap;
	uint32_t block_size;
	uint32_t recv_num;
	int rand_thresh;
	int flip_thresh;
	uint32_t trunc;
	uint32_t hard_trunc;
	uint64_t file_rotate;
	pthread_mutex_t *plock;
	pthread_cond_t *pcond;
};

static int run_threads = 1;

static void sigHandler(int s) {
	run_threads = 0;
}

static int init_rand() {
	int fd, rc;
	unsigned int seed;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		return errno;
	}
	rc = read(fd, &seed, 4);
	close(fd);
	if (rc != 4) {
		return EAGAIN;
	}
	srand(seed);
	return 0;
}

static void callback_recv(msk_trans_t *, msk_data_t *, void*);

static void callback_error(msk_trans_t *trans, msk_data_t *pdata, void* arg) {
	INFO_LOG(trans->state != MSK_CLOSING && trans->state != MSK_CLOSED
		&& trans->debug, "error callback on buffer %p", pdata);
}

static void callback_send(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	struct datalock *datalock = arg;
	struct privatedata *priv = trans->private_data;

	if (!datalock || !priv) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	pthread_mutex_lock(&datalock->lock);
	if (msk_post_recv(priv->o_trans, pdata, callback_recv, callback_error, datalock))
		ERROR_LOG("post_recv failed in send callback!");
	pthread_mutex_unlock(&datalock->lock);
}

static void callback_disconnect(msk_trans_t *trans) {
	struct privatedata *priv = trans->private_data;

	if (!priv)
		return;

	pthread_mutex_lock(priv->targ->plock);
	pthread_cond_broadcast(priv->targ->pcond);
	pthread_mutex_unlock(priv->targ->plock);
}

static void callback_recv(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	struct datalock *datalock = arg;
	struct pcap_pkthdr pcaphdr;
	struct pkt_hdr *packet;
	struct privatedata *priv = trans->private_data;

	if (!datalock || !priv) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	/* error injection */
	if (priv->targ->flip_thresh) {
		uint8_t *cur;
		int r;
		for (cur = pdata->data; cur < pdata->data + pdata->size; cur++) {
			r = rand();
			if (r < priv->targ->rand_thresh) {
				*cur = (uint8_t)r;
			} else if (r < priv->targ->flip_thresh) {
				*cur ^= r % 8;
			}
		}
	}

	pthread_mutex_lock(&datalock->lock);
	msk_post_send(priv->o_trans, pdata, callback_send, callback_error, datalock);

	gettimeofday(&pcaphdr.ts, NULL);
	pcaphdr.len = min(pdata->size + PACKET_HDR_LEN, priv->targ->hard_trunc);
	pcaphdr.caplen = min(pcaphdr.len, priv->targ->trunc);

	packet = (struct pkt_hdr*)(pdata->data - PACKET_HDR_LEN);

	/* ipv6 payload is tcp header + payload */
	packet->ipv6.ip_len = htons(pdata->size + sizeof(struct tcp_hdr));

	/* Need the lock both for writing in pcap_dumper AND for ack_nr,
	 * otherwise some seq numbers are not ordered properly.
	 */
	pthread_mutex_lock(priv->targ->plock);
	packet->tcp.th_seq_nr = priv->seq_nr;
	/* increment by the size of the tcp payload: just data */
	priv->seq_nr = htonl(ntohl(priv->seq_nr) + pdata->size);
	packet->tcp.th_ack_nr = ((struct privatedata*)priv->o_trans->private_data)->seq_nr;
	ipv6_tcp_checksum(packet);

	pcap_dump((u_char*)*priv->targ->p_pcap_dumper, &pcaphdr, (u_char*)packet);
	pthread_mutex_unlock(priv->targ->plock);
	pthread_mutex_unlock(&datalock->lock);
}

static void print_help(char **argv) {
	printf("Usage: %s -s port -c addr port [-f pcap.out]\n", argv[0]);
	printf("Mandatory arguments:\n"
		"	-c, --client addr port: connect point on incoming connection\n"
		"	-s, --server port: listen on local addresses at given port\n"
		"	OR\n"
		"	-S addr port: listen on given address/port\n"
		"Optional arguments:\n"
		"	-f, --file pcap.out: output file\n"
		"	-R, --rotate size: rotate output file (to file.1) at size\n"
		"	-t, --truncate size: size to truncate packets at (with tcp header)\n"
		"		If viewed in wireshark, max is %u (default: %u)\n"
		"	-b, --block-size size: size of packets to send (default: %u)\n"
		"	-r, --recv-num num: number of packets we can recv at once (default: %u)\n"
		"	-E, --rand-byte <proba>: with ratio between 0.0 and 1.0,\n"
		"		probability for each byte to be changed randomly\n"
		"		The data is dumped _after_ error injection\n"
		"	-e, --flip-bit <proba>: same, but there's only one bit flip\n"
		"	-v, --verbose: verbose, more v for more verbosity\n"
		"	-q, --quiet: quiet output\n",
		DEFAULT_HARD_TRUNC_LEN, DEFAULT_TRUNC_LEN,
		DEFAULT_BLOCK_SIZE, DEFAULT_RECV_NUM);

}

static void* flush_thread(void *arg) {
	struct thread_arg *thread_arg = arg;
	long pos = strlen(thread_arg->pcap_filename) + 3;
	char *backpath = alloca(pos);
	snprintf(backpath, pos, "%s.1", thread_arg->pcap_filename);
	pcap_dumper_t **p_pcap_dumper = thread_arg->p_pcap_dumper;
	/* keep this value for close or race between check in while and dump_flush */
	pcap_dumper_t *pcap_dumper = *p_pcap_dumper;

	while (*p_pcap_dumper) {
		pcap_dump_flush(pcap_dumper);
		if (thread_arg->file_rotate) {
			pos = pcap_dump_ftell(pcap_dumper);
			/* on error pos == -1 < file_rotate, just continue */
			if (pos > thread_arg->file_rotate) {
				pthread_mutex_lock(thread_arg->plock);
				pcap_dump_flush(pcap_dumper);
				pcap_dump_close(pcap_dumper);
				TEST_Z(rename(thread_arg->pcap_filename, backpath));
				TEST_NZ(pcap_dumper = pcap_dump_open(thread_arg->pcap, thread_arg->pcap_filename));
				*p_pcap_dumper = pcap_dumper;
				pthread_mutex_unlock(thread_arg->plock);
			}
		}

		sleep(1);
	}

	pcap_dump_close(pcap_dumper);

	pthread_exit(NULL);
}

static void *setup_thread(void *arg){
	uint8_t *rdmabuf;
	struct ibv_mr *mr;
	struct thread_arg *thread_arg;
	msk_data_t *data;
	struct datalock *datalock;
	struct pkt_hdr pkt_hdr;
	int i;
	struct privatedata *s_priv, *c_priv;
	msk_trans_t *child_trans, *c_trans;

	TEST_NZ(child_trans = arg);
	TEST_NZ(c_trans = child_trans->private_data);
	TEST_NZ(thread_arg = c_trans->private_data);

	const size_t mr_size = 2*(thread_arg->recv_num+1)*(thread_arg->block_size+PACKET_HDR_LEN)*sizeof(char);

	TEST_Z(msk_connect(c_trans));


	// set up data_t elements and mr (needs to be common for both as well)
	TEST_NZ(rdmabuf = malloc(mr_size));
	memset(rdmabuf, 0, mr_size);
	//FIXME that's not possible, can only reg it once -- need to use the same pd for both trans
	TEST_NZ(mr = msk_reg_mr(c_trans, rdmabuf, mr_size, IBV_ACCESS_LOCAL_WRITE));



	TEST_NZ(data = malloc(2*thread_arg->recv_num*sizeof(msk_data_t)));
	TEST_NZ(datalock = malloc(2*thread_arg->recv_num*sizeof(struct datalock)));

	memset(&pkt_hdr, 0, sizeof(pkt_hdr));

	pkt_hdr.ipv6.ip_flags[0] = 0x60; /* 6 in the leftmosts 4 bits */
	pkt_hdr.ipv6.ip_nh = IPPROTO_TCP;
	pkt_hdr.ipv6.ip_hl = 1;
	/** @todo: add options, use one of :
			CLIENT
		child_trans->cm_id->route.addr.dst_sin
		child_trans->cm_id->route.addr.src_sin
			RMITM
		c_trans->cm_id->route.addr.src_sin
		c_trans->cm_id->route.addr.dst_sin
			SERVER
	*/
	pkt_hdr.ipv6.ip_src.s6_addr16[4] = 0xffff;
	pkt_hdr.ipv6.ip_src.s6_addr16[5] = 0x0000;
	pkt_hdr.ipv6.ip_src.s6_addr32[3] = ((struct sockaddr_in*)msk_get_src_addr(c_trans))->sin_addr.s_addr;
	pkt_hdr.tcp.th_sport = msk_get_src_port(c_trans);

	pkt_hdr.ipv6.ip_dst.s6_addr16[4] = 0xffff;
	pkt_hdr.ipv6.ip_dst.s6_addr16[5] = 0x0000;
	pkt_hdr.ipv6.ip_dst.s6_addr32[3] = ((struct sockaddr_in*)msk_get_dst_addr(c_trans))->sin_addr.s_addr;
	pkt_hdr.tcp.th_dport = msk_get_dst_port(c_trans);

	pkt_hdr.tcp.th_data_off = INT8_C(sizeof(struct tcp_hdr) * 4); /* *4 because words of 2 bits? it's odd. */
	pkt_hdr.tcp.th_window = htons(100);
	pkt_hdr.tcp.th_flags = THF_ACK;

	for (i=0; i < 2*thread_arg->recv_num; i++) {
		if (i == thread_arg->recv_num) { // change packet direction
			pkt_hdr.ipv6.ip_src.s6_addr32[3] = ((struct sockaddr_in*)msk_get_dst_addr(c_trans))->sin_addr.s_addr;
			pkt_hdr.tcp.th_sport = msk_get_dst_port(c_trans);
			pkt_hdr.ipv6.ip_dst.s6_addr32[3] = ((struct sockaddr_in*)msk_get_src_addr(c_trans))->sin_addr.s_addr;
			pkt_hdr.tcp.th_dport = msk_get_src_port(c_trans);
		}
		memcpy(rdmabuf+(i)*(thread_arg->block_size+PACKET_HDR_LEN), &pkt_hdr, PACKET_HDR_LEN);
		data[i].data=rdmabuf+(i)*(thread_arg->block_size+PACKET_HDR_LEN)+PACKET_HDR_LEN;
		data[i].max_size=thread_arg->block_size;
		data[i].mr = mr;
		datalock[i].data = &data[i];
		pthread_mutex_init(&datalock[i].lock, NULL);
	}

	// set up the data needed to communicate
	TEST_NZ(child_trans->private_data = malloc(sizeof(struct privatedata)));
	TEST_NZ(c_trans->private_data = malloc(sizeof(struct privatedata)));
	s_priv = child_trans->private_data;
	c_priv = c_trans->private_data;

	s_priv->targ = thread_arg;
	c_priv->targ = thread_arg;

	c_priv->seq_nr = pkt_hdr.tcp.th_seq_nr;
	s_priv->seq_nr = pkt_hdr.tcp.th_seq_nr;

	s_priv->o_trans = c_trans;
	c_priv->o_trans = child_trans;

	s_priv->first_datalock = datalock;
	c_priv->first_datalock = datalock + thread_arg->recv_num;

	for (i=0; i<thread_arg->recv_num; i++) {
		TEST_Z(msk_post_recv(c_trans, (&c_priv->first_datalock[i])->data, callback_recv, callback_error, &c_priv->first_datalock[i]));
		TEST_Z(msk_post_recv(child_trans, (&s_priv->first_datalock[i])->data, callback_recv, callback_error, &s_priv->first_datalock[i]));
	}

	pthread_mutex_lock(thread_arg->plock);

	/* finalize_connect first, finalize_accept second */
	TEST_Z(msk_finalize_connect(c_trans));
	TEST_Z(msk_finalize_accept(child_trans));

	ERROR_LOG("New connection setup\n");

	/* Wait till either connection has a disconnect callback */
	while (c_trans->state == MSK_CONNECTED &&
	       child_trans->state == MSK_CONNECTED) {
		pthread_cond_wait(thread_arg->pcond, thread_arg->plock);
	}
	pthread_mutex_unlock(thread_arg->plock);

	msk_destroy_trans(&c_trans);
	msk_destroy_trans(&child_trans);

	free(c_priv);
	free(s_priv);
	free(datalock);
	free(data);
	free(rdmabuf);

	pthread_exit(NULL);
}

int main(int argc, char **argv) {
	msk_trans_t *s_trans;
	msk_trans_t *child_trans;
	msk_trans_t *c_trans;
	msk_trans_attr_t s_attr;
	msk_trans_attr_t c_attr;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	pthread_t thrid, flushthrid;

	pcap_t *pcap;
	pcap_dumper_t *pcap_dumper;
	double double_proba;

	// argument handling
	struct thread_arg thread_arg;
	int option_index = 0;
	int op, last_op;
	char *tmp_s;
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "help",	no_argument,		0,		'h' },
		{ "verbose",	no_argument,		0,		'v' },
		{ "quiet",	no_argument,		0,		'q' },
		{ "block-size",	required_argument,	0,		'b' },
		{ "recv-num",	required_argument,	0,		'r' },
		{ "rotate",	required_argument,	0,		'R' },
		{ "file",	required_argument,	0,		'f' },
		{ "truncate",	required_argument,	0,		't' },
		{ "rand-byte",	required_argument,	0,		'E' },
		{ "flip-bit",	required_argument,	0,		'e' },
		{ 0,		0,			0,		 0  }
	};


	memset(&s_attr, 0, sizeof(msk_trans_attr_t));
	memset(&c_attr, 0, sizeof(msk_trans_attr_t));
	memset(&thread_arg, 0, sizeof(struct thread_arg));

	s_attr.server = -1; // put an incorrect value to check if we're either client or server
	c_attr.server = -1;
	// sane values for optional or non-configurable elements
	s_attr.debug = 1;
	c_attr.debug = 1;
	s_attr.max_recv_sge = 1;
	s_attr.disconnect_callback = callback_disconnect;
	c_attr.max_recv_sge = 1;
	c_attr.disconnect_callback = callback_disconnect;
	s_attr.worker_count = -1;
	c_attr.worker_count = -1;
	thread_arg.pcap_filename = "pcap.out";

	last_op = 0;
	while ((op = getopt_long(argc, argv, "-@hvqE:e:s:S:c:w:b:f:r:t:R:", long_options, &option_index)) != -1) {
		switch(op) {
			case 1: // this means double argument
				if (last_op == 'c') {
					c_attr.port = optarg;
				} else if (last_op == 'S') {
					s_attr.port = optarg;
				} else {
					ERROR_LOG("Failed to parse arguments");
					print_help(argv);
					exit(EINVAL);
				}
				break;
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
				c_attr.debug = c_attr.debug * 2 + 1;
				s_attr.debug = c_attr.debug;
				break;
			case 'q':
				c_attr.debug = 0;
				s_attr.debug = 0;
				break;
			case 'c':
				c_attr.server = 0;
				c_attr.node = optarg;
				break;
			case 's':
				s_attr.server = 10;
				s_attr.node = "::";
				s_attr.port = optarg;
				break;
			case 'S':
				s_attr.server = 10;
				s_attr.node = optarg;
				break;
			case 'w':
				ERROR_LOG("-w has become deprecated, use -f or --file now. Proceeding anyway");
				/* fallthrough */
			case 'f':
				thread_arg.pcap_filename = optarg;
				break;
			case 'R':
				thread_arg.file_rotate = strtoull(optarg, &tmp_s, 0);
				if (errno || thread_arg.file_rotate == 0) {
					ERROR_LOG("Invalid rotate length, assuming no truncate");
					break;
				}
				if (tmp_s[0] != 0) {
					set_size(thread_arg.file_rotate, tmp_s);
				}
				INFO_LOG(c_attr.debug >1, "rotate length: %"PRIu64, thread_arg.file_rotate);

				break;
			case 't':
				thread_arg.trunc = strtoul(optarg, &tmp_s, 0);
				if (errno || thread_arg.trunc == 0) {
					ERROR_LOG("Invalid truncate length, assuming default (%u)", DEFAULT_TRUNC_LEN);
					break;
				}
				if (tmp_s[0] != 0) {
					set_size(thread_arg.trunc, tmp_s);
				}
				INFO_LOG(c_attr.debug > 1, "truncate length: %u", thread_arg.trunc);
				break;
			case 'b':
				thread_arg.block_size = strtoul(optarg, &tmp_s, 0);
				if (errno || thread_arg.block_size == 0) {
					ERROR_LOG("Invalid block size, assuming default (%u)", DEFAULT_BLOCK_SIZE);
					break;
				}
				if (tmp_s[0] != 0) {
					set_size(thread_arg.block_size, tmp_s);
				}
				INFO_LOG(c_attr.debug > 1, "block size: %u", thread_arg.block_size);
				break;
			case 'r':
				thread_arg.recv_num = strtoul(optarg, NULL, 0);

				if (errno || thread_arg.recv_num == 0)
					ERROR_LOG("Invalid recv_num, assuming default (%u)", DEFAULT_RECV_NUM);
				break;
			case 'E':
				double_proba = strtod(optarg, &tmp_s);
				if (tmp_s == optarg || double_proba < 0.0 || double_proba > 1.0) {
					ERROR_LOG("probability \"%s\" must be between 0.0 and 1.0\n",
						optarg);
					exit(EINVAL);
				}

				thread_arg.rand_thresh = RAND_MAX*double_proba;
				break;
			case 'e':
				double_proba = strtod(optarg, &tmp_s);
				if (tmp_s == optarg || double_proba < 0.0 || double_proba > 1.0) {
					ERROR_LOG("probability \"%s\" must be between 0.0 and 1.0\n",
						optarg);
					exit(EINVAL);
				}

				thread_arg.flip_thresh = RAND_MAX*double_proba;
				break;
			default:
				ERROR_LOG("Failed to parse arguments");
				print_help(argv);
				exit(EINVAL);
		}
		last_op = op;
	}

	if (c_attr.server == -1 || s_attr.server == -1) {
		ERROR_LOG("must have both client and server!");
		print_help(argv);
		exit(EINVAL);
	}

	if (thread_arg.file_rotate && strncmp(thread_arg.pcap_filename, "-", 2) == 0) {
		ERROR_LOG("Can't rotate stdout!");
		print_help(argv);
		exit(EINVAL);
	}

	if (thread_arg.block_size == 0)
		thread_arg.block_size = DEFAULT_BLOCK_SIZE;
	if (thread_arg.recv_num == 0)
		thread_arg.recv_num = DEFAULT_RECV_NUM;
	if (thread_arg.trunc == 0)
		thread_arg.trunc = DEFAULT_TRUNC_LEN;
	thread_arg.hard_trunc = max(thread_arg.trunc, DEFAULT_HARD_TRUNC_LEN);

	if (thread_arg.flip_thresh > RAND_MAX - thread_arg.rand_thresh) {
		ERROR_LOG("flip and random probabilities are additive, can't be more than 1!");
		exit(EINVAL);
	}
	thread_arg.flip_thresh += thread_arg.rand_thresh;

	s_attr.rq_depth = thread_arg.recv_num+1;
	s_attr.sq_depth = thread_arg.recv_num+1;
	c_attr.rq_depth = thread_arg.recv_num+1;
	c_attr.sq_depth = thread_arg.recv_num+1;

	TEST_Z(init_rand());

	// server init
	TEST_Z(msk_init(&s_trans, &s_attr));

	if (!s_trans)
		exit(-1);

	TEST_Z(msk_bind_server(s_trans));

	pcap = pcap_open_dead(DLT_RAW, thread_arg.hard_trunc);
	TEST_NZ(pcap_dumper = pcap_dump_open(pcap, thread_arg.pcap_filename));
	TEST_Z(pthread_create(&flushthrid, NULL, flush_thread, &thread_arg));

	thread_arg.pcap = pcap;
	thread_arg.p_pcap_dumper = &pcap_dumper;

	memset(&lock, 0, sizeof(pthread_mutex_t));
	memset(&cond, 0, sizeof(pthread_cond_t));
	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);
	thread_arg.plock = &lock;
	thread_arg.pcond = &cond;

	signal(SIGINT, sigHandler);
	signal(SIGHUP, sigHandler);

	while (run_threads) {
		child_trans = msk_accept_one_wait(s_trans, 1000);

		if (child_trans == NULL)
			continue;

		// got a client, start our own client before we finalize the server's connection
		c_attr.pd = child_trans->pd;
		TEST_Z(msk_init(&c_trans, &c_attr));
		if (!c_trans) {
			ERROR_LOG("Couldn't connect to remote endpoint, aborting");
			break;
		}

		// ugly hack to have all the arguments we need given...
		child_trans->private_data = c_trans;
		c_trans->private_data = &thread_arg;
		TEST_Z(pthread_create(&thrid, NULL, setup_thread, child_trans));
	}

	pcap_dumper = NULL;
	pthread_join(flushthrid, NULL);

	pcap_close(pcap);

	pthread_cond_destroy(&cond);
	pthread_mutex_destroy(&lock);

	msk_destroy_trans(&s_trans);

	return 0;
}

