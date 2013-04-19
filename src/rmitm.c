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
#include <getopt.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>	//open
#include <sys/stat.h>	//open
#include <fcntl.h>	//open

#include <pcap.h>
#include <linux/if_arp.h>

#include "log.h"
#include "mooshika.h"
#include "rmitm.h"

#define CHUNK_SIZE 1024*1024 // nfs page size
#define RECV_NUM 4
#define NUM_SGE 1
#define PACKET_HARD_MAX_LEN (64*1024-1)
#define PACKET_TRUNC_LEN 1000

#define TEST_Z(x)  do { if ( (x)) { ERROR_LOG("error: " #x " failed (returned non-zero)." ); exit(-1); }} while (0)
#define TEST_NZ(x) do { if (!(x)) { ERROR_LOG("error: " #x " failed (returned zero/null)."); exit(-1); }} while (0)

struct privatedata {
	pcap_dumper_t *pcap_dumper;
	uint32_t seq_nr;
	msk_trans_t *o_trans;
	struct ibv_mr *mr;
	msk_data_t *first_rdata;
	msk_data_t *first_data;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_recv(msk_trans_t *, void*);

void callback_send(msk_trans_t *trans, void *arg) {
	msk_data_t *data = arg;
	struct privatedata *priv = trans->private_data;

	if (!data || !priv) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	msk_post_n_recv(priv->o_trans, data, NUM_SGE, priv->mr, callback_recv, data);
}

void callback_disconnect(msk_trans_t *trans) {
	struct privatedata *priv = trans->private_data;

	if (!priv)
		return;

	pthread_mutex_lock(priv->lock);
	pthread_cond_broadcast(priv->cond);
	pthread_mutex_unlock(priv->lock);
}

void callback_recv(msk_trans_t *trans, void *arg) {
	msk_data_t *data = arg;
	struct pcap_pkthdr pcaphdr;
	struct pkt_hdr *packet;
	struct privatedata *priv = trans->private_data;

	if (!data || !priv) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	msk_post_send(priv->o_trans, data, priv->mr, callback_send, data);

	gettimeofday(&pcaphdr.ts, NULL);
	pcaphdr.len = min(data->size + PACKET_HDR_LEN, PACKET_HARD_MAX_LEN);
	pcaphdr.caplen = min(pcaphdr.len, PACKET_TRUNC_LEN);

	packet = (struct pkt_hdr*)(data->data - PACKET_HDR_LEN);

	packet->ipv6.ip_len = htons(pcaphdr.len);
	packet->tcp.th_seq_nr = priv->seq_nr;
	priv->seq_nr = htonl(ntohl(priv->seq_nr) + pcaphdr.len - sizeof(struct pkt_hdr));
	packet->tcp.th_ack_nr = ((struct privatedata*)priv->o_trans->private_data)->seq_nr;

	pthread_mutex_lock(priv->lock);
	pcap_dump((u_char*)priv->pcap_dumper, &pcaphdr, (u_char*)packet);
	pthread_mutex_unlock(priv->lock);
}

void print_help(char **argv) {
	printf("Usage: %s -s port -c addr port\n", argv[0]);
}

void* flush_thread(void *arg) {
	pcap_dumper_t **p_pcap_dumper = arg;

	while (*p_pcap_dumper) {
		sleep(1);
		pcap_dump_flush(*p_pcap_dumper);
	}

	pthread_exit(NULL);
}

void* handle_trans(void *arg) {
	msk_trans_t *trans = arg;
	struct privatedata *priv;
	msk_trans_t *o_trans;
	struct ibv_mr *mr;
	int i;

	TEST_NZ(priv = trans->private_data);
	TEST_NZ(o_trans = priv->o_trans);
	TEST_NZ(mr = priv->mr);

	for (i=0; i<RECV_NUM; i++)
		TEST_Z(msk_post_n_recv(trans, &priv->first_rdata[NUM_SGE*i], NUM_SGE, mr, callback_recv, &priv->first_rdata[NUM_SGE*i]));

	printf("%s: done posting recv buffers\n", trans->server ? "server" : "client");

	/* finalize_connect first, finalize_accept second */
	if (!trans->server) {
		pthread_mutex_lock(priv->lock);
		TEST_Z(msk_finalize_connect(trans));
		pthread_cond_signal(priv->cond);
	} else {
		pthread_cond_wait(priv->cond, priv->lock);
		TEST_Z(msk_finalize_accept(trans));
	}


	/* Wait till either connection has a disconnect callback */
	pthread_cond_wait(priv->cond, priv->lock);
	pthread_mutex_unlock(priv->lock);

	msk_destroy_trans(&trans);

	pthread_exit(NULL);
}

int main(int argc, char **argv) {


	msk_trans_t *s_trans;
	msk_trans_t *child_trans;
	msk_trans_t *c_trans;

	msk_trans_attr_t s_attr;
	msk_trans_attr_t c_attr;

	memset(&s_attr, 0, sizeof(msk_trans_attr_t));
	memset(&c_attr, 0, sizeof(msk_trans_attr_t));

	s_attr.server = -1; // put an incorrect value to check if we're either client or server
	c_attr.server = -1;
	// sane values for optional or non-configurable elements
	s_attr.rq_depth = RECV_NUM+1;
	s_attr.sq_depth = RECV_NUM+1;
	s_attr.max_recv_sge = NUM_SGE;
	s_attr.addr.sa_in.sin_family = AF_INET;
	s_attr.disconnect_callback = callback_disconnect;
	c_attr.rq_depth = RECV_NUM+1;
	c_attr.sq_depth = RECV_NUM+1;
	c_attr.max_recv_sge = NUM_SGE;
	c_attr.addr.sa_in.sin_family = AF_INET;
	c_attr.disconnect_callback = callback_disconnect;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "help",	no_argument,		0,		'h' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op, last_op;
	last_op = 0;
	while ((op = getopt_long(argc, argv, "-@hvs:S:c:", long_options, &option_index)) != -1) {
		switch(op) {
			case 1: // this means double argument
				if (last_op == 'c') {
					c_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				} else if (last_op == 'S') {
					s_attr.addr.sa_in.sin_port = htons(atoi(optarg));
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
				ERROR_LOG("verbose switch not ready just yet, come back later!\n");
				break;
			case 'c':
				c_attr.server = 0;
				inet_pton(AF_INET, optarg, &c_attr.addr.sa_in.sin_addr);
				break;
			case 's':
				s_attr.server = 10;
				inet_pton(AF_INET, "0.0.0.0", &s_attr.addr.sa_in.sin_addr);
				s_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				break;
			case 'S':
				s_attr.server = 10;
				inet_pton(AF_INET, optarg, &s_attr.addr.sa_in.sin_addr);
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


	// server init
	TEST_Z(msk_init(&s_trans, &s_attr));

	if (!s_trans)
		exit(-1);

	TEST_Z(msk_bind_server(s_trans));
	child_trans = msk_accept_one(s_trans);
	
	// got a client, start our own client before we finalize the server's connection

	c_attr.pd = child_trans->pd;

	TEST_Z(msk_init(&c_trans, &c_attr));

	if (!c_trans)
		exit(-1);

	TEST_Z(msk_connect(c_trans));


	// set up data_t elements and mr (needs to be common for both as well)

	uint8_t *rdmabuf;
	struct ibv_mr *mr;

	const size_t mr_size = 2*(RECV_NUM+1)*NUM_SGE*(CHUNK_SIZE+PACKET_HDR_LEN)*sizeof(char);

	TEST_NZ(rdmabuf = malloc(mr_size));
	memset(rdmabuf, 0, mr_size);
	//FIXME that's not possible, can only reg it once -- need to use the same pd for both trans
	TEST_NZ(mr = msk_reg_mr(c_trans, rdmabuf, mr_size, IBV_ACCESS_LOCAL_WRITE));


	msk_data_t *rdata;
	int i, j;

	TEST_NZ(rdata = malloc(NUM_SGE*2*RECV_NUM*sizeof(msk_data_t*)*sizeof(msk_data_t)));

	struct pkt_hdr pkt_hdr;

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
	pkt_hdr.ipv6.ip_src.s6_addr32[3] = c_trans->cm_id->route.addr.dst_sin.sin_addr.s_addr;
	pkt_hdr.tcp.th_sport = child_trans->cm_id->route.addr.src_sin.sin_port;

	pkt_hdr.ipv6.ip_dst.s6_addr16[4] = 0xffff;
	pkt_hdr.ipv6.ip_dst.s6_addr16[5] = 0x0000;
	pkt_hdr.ipv6.ip_dst.s6_addr32[3] = child_trans->cm_id->route.addr.dst_sin.sin_addr.s_addr;
	pkt_hdr.tcp.th_dport = child_trans->cm_id->route.addr.dst_sin.sin_port;

	pkt_hdr.tcp.th_data_off = INT8_C(sizeof(struct tcp_hdr) * 4); /* *4 because words of 2 bits? it's odd. */
	pkt_hdr.tcp.th_window = htons(100);
	pkt_hdr.tcp.th_flags = THF_ACK;

	for (i=0; i < 2*RECV_NUM; i++) {
		if (i == RECV_NUM) { // change packet direction
			pkt_hdr.ipv6.ip_src.s6_addr32[3] = child_trans->cm_id->route.addr.dst_sin.sin_addr.s_addr;
			pkt_hdr.tcp.th_sport = child_trans->cm_id->route.addr.dst_sin.sin_port;
			pkt_hdr.ipv6.ip_dst.s6_addr32[3] = c_trans->cm_id->route.addr.dst_sin.sin_addr.s_addr;
			pkt_hdr.tcp.th_dport = child_trans->cm_id->route.addr.src_sin.sin_port;
		}
		for (j=0; j<NUM_SGE; j++) {
			memcpy(rdmabuf+(NUM_SGE*i+j)*(CHUNK_SIZE+PACKET_HDR_LEN), &pkt_hdr, PACKET_HDR_LEN);
			rdata[NUM_SGE*i+j].data=rdmabuf+(NUM_SGE*i+j)*(CHUNK_SIZE+PACKET_HDR_LEN)+PACKET_HDR_LEN;
			rdata[NUM_SGE*i+j].max_size=CHUNK_SIZE;
		}
	}

	// set up the data needed to communicate
	TEST_NZ(child_trans->private_data = malloc(sizeof(struct privatedata)));
	TEST_NZ(c_trans->private_data = malloc(sizeof(struct privatedata)));
	struct privatedata *s_priv, *c_priv;
	s_priv = child_trans->private_data;
	c_priv = c_trans->private_data;


	pcap_t *pcap = pcap_open_dead(DLT_RAW, PACKET_HARD_MAX_LEN);
	s_priv->pcap_dumper = pcap_dump_open(pcap, "/tmp/rmitm.pcap");
	c_priv->pcap_dumper = s_priv->pcap_dumper;

	c_priv->seq_nr = pkt_hdr.tcp.th_seq_nr;
	s_priv->seq_nr = pkt_hdr.tcp.th_seq_nr;

	s_priv->o_trans = c_trans;
	c_priv->o_trans = child_trans;

	s_priv->first_rdata    = rdata;
	s_priv->mr             = mr;
	c_priv->first_rdata    = rdata + NUM_SGE*RECV_NUM;
	c_priv->mr             = mr;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);
	c_priv->lock = &lock;
	s_priv->lock = &lock;
	s_priv->cond = &cond;
	c_priv->cond = &cond;


	pthread_t s_threadid, c_threadid, flushthrid;
	pthread_mutex_lock(c_priv->lock);
	pthread_create(&s_threadid, NULL, handle_trans, child_trans);
	pthread_create(&c_threadid, NULL, handle_trans, c_trans);
	pthread_create(&flushthrid, NULL, flush_thread, &c_priv->pcap_dumper);

	pthread_join(s_threadid, NULL);
	pthread_join(c_threadid, NULL);

	c_priv->pcap_dumper = NULL;
	pthread_join(flushthrid, NULL);

	printf("closing stuff!\n");

	pcap_dump_close(s_priv->pcap_dumper);

	pthread_cond_destroy(&cond);
	pthread_mutex_destroy(&lock);
	free(c_priv);
	free(s_priv);
/*	for (i=0; i<2*RECV_NUM; i++)
		free(rdata[i]); */
	free(rdata);
	free(rdmabuf);

	msk_destroy_trans(&s_trans);

	return 0;
}

