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

#include <pcap/pcap.h>
#include <linux/if_arp.h>

#include "utils.h"
#include "mooshika.h"
#include "rmitm.h"

#define DEFAULT_BLOCK_SIZE 1024*1024 // nfs page size
#define RECV_NUM 4
#define PACKET_HARD_MAX_LEN (64*1024-1)
#define PACKET_TRUNC_LEN 1000

#define TEST_Z(x)  do { if ( (x)) { ERROR_LOG("error: " #x " failed (returned non-zero)." ); exit(-1); }} while (0)
#define TEST_NZ(x) do { if (!(x)) { ERROR_LOG("error: " #x " failed (returned zero/null)."); exit(-1); }} while (0)


struct datalock {
	msk_data_t *data;
	pthread_mutex_t lock;
};

struct privatedata {
	pcap_dumper_t *pcap_dumper;
	uint32_t seq_nr;
	msk_trans_t *o_trans;
	struct datalock *first_datalock;
	pthread_mutex_t *plock;
	pthread_cond_t *pcond;
};

struct thread_arg {
	pcap_dumper_t *pcap_dumper;
	uint32_t block_size;
};

static int run_threads = 1;

void sigHandler(int s) {
	run_threads = 0;
}

void callback_recv(msk_trans_t *, msk_data_t *, void*);

void callback_error(msk_trans_t *trans, msk_data_t *pdata, void* arg) {
}

void callback_send(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
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

void callback_disconnect(msk_trans_t *trans) {
	struct privatedata *priv = trans->private_data;

	if (!priv)
		return;

	pthread_mutex_lock(priv->plock);
	pthread_cond_broadcast(priv->pcond);
	pthread_mutex_unlock(priv->plock);
}

void callback_recv(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	struct datalock *datalock = arg;
	struct pcap_pkthdr pcaphdr;
	struct pkt_hdr *packet;
	struct privatedata *priv = trans->private_data;

	if (!datalock || !priv) {
		ERROR_LOG("no callback_arg?");
		return;
	}

	pthread_mutex_lock(&datalock->lock);
	msk_post_send(priv->o_trans, pdata, callback_send, callback_error, datalock);

	gettimeofday(&pcaphdr.ts, NULL);
	pcaphdr.len = min(pdata->size + PACKET_HDR_LEN, PACKET_HARD_MAX_LEN);
	pcaphdr.caplen = min(pcaphdr.len, PACKET_TRUNC_LEN);

	packet = (struct pkt_hdr*)(pdata->data - PACKET_HDR_LEN);

	packet->ipv6.ip_len = htons(pcaphdr.len);
	packet->tcp.th_seq_nr = priv->seq_nr;

	/* Need the lock both for writing in pcap_dumper AND for ack_nr,
	 * otherwise some seq numbers are not ordered properly.
	 */
	pthread_mutex_lock(priv->plock);
	priv->seq_nr = htonl(ntohl(priv->seq_nr) + pcaphdr.len - sizeof(struct pkt_hdr));
	packet->tcp.th_ack_nr = ((struct privatedata*)priv->o_trans->private_data)->seq_nr;
	ipv6_tcp_checksum(packet);

	pcap_dump((u_char*)priv->pcap_dumper, &pcaphdr, (u_char*)packet);
	pthread_mutex_unlock(priv->plock);
	pthread_mutex_unlock(&datalock->lock);
}

void print_help(char **argv) {
	printf("Usage: %s -s port -c addr port [-w out.pcap] [-b blocksize]\n", argv[0]);
	printf("Mandatory arguments:\n"
		"	-c, --client addr port: connect point on incoming connection\n"
		"	-s, --server port: listen on local addresses at given port\n"
		"	OR\n"
		"	-S addr port: listen on given address/port\n"
		"Optional arguments:\n"
		"	-w out.pcap: output file\n"
		"	-b, --block-size size: size of packets to send (default: %u)\n", DEFAULT_BLOCK_SIZE);

}

void* flush_thread(void *arg) {
	pcap_dumper_t **p_pcap_dumper = arg;
	/* keep this value for close or race between check in while and dump_flush */
	pcap_dumper_t *pcap_dumper = *p_pcap_dumper;

	while (*p_pcap_dumper) {
		pcap_dump_flush(pcap_dumper);
		sleep(1);
	}

	pcap_dump_close(pcap_dumper);

	pthread_exit(NULL);
}

void *setup_thread(void *arg){
	uint8_t *rdmabuf;
	struct ibv_mr *mr;
	struct thread_arg *thread_arg;
	msk_data_t *data;
	struct datalock *datalock;
	struct pkt_hdr pkt_hdr;
	int i;
	struct privatedata *s_priv, *c_priv;
	msk_trans_t *child_trans, *c_trans;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	TEST_NZ(child_trans = arg);
	TEST_NZ(c_trans = child_trans->private_data);
	TEST_NZ(thread_arg = c_trans->private_data);

	const size_t mr_size = 2*(RECV_NUM+1)*(thread_arg->block_size+PACKET_HDR_LEN)*sizeof(char);

	TEST_Z(msk_connect(c_trans));


	// set up data_t elements and mr (needs to be common for both as well)
	TEST_NZ(rdmabuf = malloc(mr_size));
	memset(rdmabuf, 0, mr_size);
	//FIXME that's not possible, can only reg it once -- need to use the same pd for both trans
	TEST_NZ(mr = msk_reg_mr(c_trans, rdmabuf, mr_size, IBV_ACCESS_LOCAL_WRITE));



	TEST_NZ(data = malloc(2*RECV_NUM*sizeof(msk_data_t)));
	TEST_NZ(datalock = malloc(2*RECV_NUM*sizeof(struct datalock)));

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

	for (i=0; i < 2*RECV_NUM; i++) {
		if (i == RECV_NUM) { // change packet direction
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


	s_priv->pcap_dumper = thread_arg->pcap_dumper;
	c_priv->pcap_dumper = thread_arg->pcap_dumper;

	c_priv->seq_nr = pkt_hdr.tcp.th_seq_nr;
	s_priv->seq_nr = pkt_hdr.tcp.th_seq_nr;

	s_priv->o_trans = c_trans;
	c_priv->o_trans = child_trans;

	s_priv->first_datalock = datalock;
	c_priv->first_datalock = datalock + RECV_NUM;

	memset(&lock, 0, sizeof(pthread_mutex_t));
	memset(&cond, 0, sizeof(pthread_cond_t));
	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);
	c_priv->plock = &lock;
	s_priv->plock = &lock;
	s_priv->pcond = &cond;
	c_priv->pcond = &cond;


	for (i=0; i<RECV_NUM; i++) {
		TEST_Z(msk_post_recv(c_trans, (&c_priv->first_datalock[i])->data, callback_recv, callback_error, &c_priv->first_datalock[i]));
		TEST_Z(msk_post_recv(child_trans, (&s_priv->first_datalock[i])->data, callback_recv, callback_error, &s_priv->first_datalock[i]));
	}

	pthread_mutex_lock(&lock);

	/* finalize_connect first, finalize_accept second */
	TEST_Z(msk_finalize_connect(c_trans));
	TEST_Z(msk_finalize_accept(child_trans));

	ERROR_LOG("New connection setup\n");

	/* Wait till either connection has a disconnect callback */
	pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);

	msk_destroy_trans(&c_trans);
	msk_destroy_trans(&child_trans);

	pthread_cond_destroy(&cond);
	pthread_mutex_destroy(&lock);
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

	pthread_t thrid, flushthrid;

	char *pcap_file;
	pcap_t *pcap;
	pcap_dumper_t *pcap_dumper;

	// argument handling
	struct thread_arg thread_arg;
	int option_index = 0;
	int op, last_op;
	char *tmp_s;
	struct hostent *host;
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "help",	no_argument,		0,		'h' },
		{ "verbose",	no_argument,		0,		'v' },
		{ "block-size",	required_argument,	0,		'b' },
		{ 0,		0,			0,		 0  }
	};


	memset(&s_attr, 0, sizeof(msk_trans_attr_t));
	memset(&c_attr, 0, sizeof(msk_trans_attr_t));
	memset(&thread_arg, 0, sizeof(struct thread_arg));

	s_attr.server = -1; // put an incorrect value to check if we're either client or server
	c_attr.server = -1;
	// sane values for optional or non-configurable elements
	s_attr.rq_depth = RECV_NUM+1;
	s_attr.sq_depth = RECV_NUM+1;
	s_attr.max_recv_sge = 1;
	s_attr.addr.sa_in.sin_family = AF_INET;
	s_attr.disconnect_callback = callback_disconnect;
	c_attr.rq_depth = RECV_NUM+1;
	c_attr.sq_depth = RECV_NUM+1;
	c_attr.max_recv_sge = 1;
	c_attr.addr.sa_in.sin_family = AF_INET;
	c_attr.disconnect_callback = callback_disconnect;
	s_attr.worker_count = -1;
	c_attr.worker_count = -1;
	pcap_file = "/tmp/rmitm.pcap";

	last_op = 0;
	while ((op = getopt_long(argc, argv, "-@hvs:S:c:w:", long_options, &option_index)) != -1) {
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
				c_attr.debug = 1;
				s_attr.debug = 1;
				break;
			case 'c':
				c_attr.server = 0;
				host = gethostbyname(optarg);
				//FIXME: if (host->h_addrtype == AF_INET6) {
				memcpy(&c_attr.addr.sa_in.sin_addr, host->h_addr_list[0], 4);
				break;
			case 's':
				s_attr.server = 10;
				inet_pton(AF_INET, "0.0.0.0", &s_attr.addr.sa_in.sin_addr);
				s_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				break;
			case 'S':
				s_attr.server = 10;
				host = gethostbyname(optarg);
				//FIXME: if (host->h_addrtype == AF_INET6) {
				memcpy(&s_attr.addr.sa_in.sin_addr, host->h_addr_list[0], 4);
				break;
			case 'w':
				pcap_file = optarg;
				break;
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
				INFO_LOG(c_attr.debug, "block size: %u", thread_arg.block_size);
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

	pcap = pcap_open_dead(DLT_RAW, PACKET_HARD_MAX_LEN);
	pcap_dumper = pcap_dump_open(pcap, pcap_file);
	TEST_Z(pthread_create(&flushthrid, NULL, flush_thread, &pcap_dumper));

	thread_arg.pcap_dumper = pcap_dumper;
	if (thread_arg.block_size == 0)
		thread_arg.block_size = DEFAULT_BLOCK_SIZE;

	signal(SIGINT, sigHandler);

	while (run_threads) {
		child_trans = msk_accept_one_wait(s_trans, 100);

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

	msk_destroy_trans(&s_trans);

	return 0;
}

