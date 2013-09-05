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
 * \file   rreplay.c
 * \brief  Replays and checks what came out of a dump file
 *
 * Replays and checks what came out of a dump file
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
#define DEFAULT_RECV_NUM 16

#define TEST_Z(x)  do { if ( (x)) { ERROR_LOG("error: " #x " failed (returned non-zero)." ); exit(-1); }} while (0)
#define TEST_NZ(x) do { if (!(x)) { ERROR_LOG("error: " #x " failed (returned zero/null)."); exit(-1); }} while (0)


struct datalock {
	msk_data_t *data;
	pthread_mutex_t lock;
};

struct privatedata {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct pcap_pkthdr *pcaphdr;
	struct pkt_hdr *packet;
	int rc;
	int docheck;
};


static void callback_error(msk_trans_t *trans, msk_data_t *pdata, void* arg) {
	INFO_LOG(trans->state != MSK_CLOSING && trans->state != MSK_CLOSED
		&& trans->debug, "error callback on buffer %p", pdata);
}

static void callback_send(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	INFO_LOG(trans->debug & MSK_DEBUG_SEND,"Sent something");
}

static void callback_disconnect(msk_trans_t *trans) {
	struct privatedata *priv = trans->private_data;

	if (!priv)
		return;

	pthread_mutex_lock(&priv->lock);
	priv->rc = ENOTCONN;
	pthread_cond_broadcast(&priv->cond);
	pthread_mutex_unlock(&priv->lock);
}

static void callback_recv(msk_trans_t *trans, msk_data_t *pdata, void *arg) {
	struct privatedata *priv = trans->private_data;

	if (!priv) {
		ERROR_LOG("no private data?");
		return;
	}

	INFO_LOG(trans->debug & MSK_DEBUG_RECV, "Received something");

	pthread_mutex_lock(&priv->lock);


	/* check we got what expected */
	if (priv->docheck && (
//	    priv->pcaphdr->len - PACKET_HDR_LEN != min(pdata->size + PACKET_HDR_LEN, PACKET_HARD_MAX_LEN) ||
	    memcmp(priv->packet->data, pdata->data, priv->pcaphdr->caplen - PACKET_HDR_LEN) != 0)) {
		ERROR_LOG("Received packet doesn't match what we expected! Aborting.");
		priv->rc = EBADMSG;
	/* only repost buffer if we didn't have an error (or didn't check) */
	} else if ((priv->rc = msk_post_recv(trans, pdata, callback_recv, callback_error, NULL))) {
		ERROR_LOG("Couldn't repost recv buffer, rc %d (%s)", priv->rc, strerror(priv->rc));
	}

	pthread_cond_signal(&priv->cond);

	pthread_mutex_unlock(&priv->lock);
}

static void print_help(char **argv) {
	printf("Usage: %s (-s port|-c addr port) [-f pcap.out]\n", argv[0]);
	printf("Mandatory argument:\n"
		"	-c, --client addr port: connect point on incoming connection\n"
		"	OR\n"
		"	-s, --server port: listen on local addresses at given port\n"
		"	OR\n"
		"	-S addr port: listen on given address/port\n"
		"Optional arguments:\n"
		"	-f, --file pcap.out: input file\n"
		"	-B, --banner: server sends a banner and is expected to talk first\n"
		"	-n, --no-check: do not verify that received data is what we expected\n"
		"	-b, --block-size size: size of packets to send (default: %u)\n"
		"	-r, --recv-num num: number of packets we can recv at once (default: %u)\n"
		"	-v, --verbose: verbose, more v for more verbosity\n"
		"	-q, --quiet: quiet output\n",
		DEFAULT_BLOCK_SIZE, DEFAULT_RECV_NUM);

}

int main(int argc, char **argv) {
	msk_trans_t *trans, *listen_trans;
	msk_trans_attr_t trans_attr;

	char errbuf[PCAP_ERRBUF_SIZE];
	char *pcap_file;
	pcap_t *pcap;

	uint32_t block_size = 0, recv_num = 0;
	int banner = 0;

	int i, rc;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;
	msk_data_t *data, *wdata;
	struct privatedata priv;


	// argument handling
	int option_index = 0;
	int op, last_op;
	char *tmp_s;
	struct hostent *host;
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "banner",	no_argument,		0,		'B' },
		{ "help",	no_argument,		0,		'h' },
		{ "verbose",	no_argument,		0,		'v' },
		{ "quiet",	no_argument,		0,		'q' },
		{ "block-size",	required_argument,	0,		'b' },
		{ "file",	required_argument,	0,		'f' },
		{ "recv-num",	required_argument,	0,		'r' },
		{ "no-check",	no_argument,		0,		'n' },
		{ 0,		0,			0,		 0  }
	};


	memset(&trans_attr, 0, sizeof(msk_trans_attr_t));
	memset(&priv, 0, sizeof(struct privatedata));
	priv.docheck = 1;

	trans_attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	trans_attr.debug = 1;
	trans_attr.max_recv_sge = 1;
	trans_attr.addr.sa_in.sin_family = AF_INET;
	trans_attr.disconnect_callback = callback_disconnect;
	trans_attr.worker_count = -1;
	pcap_file = "pcap.out";

	last_op = 0;
	while ((op = getopt_long(argc, argv, "-@hvqc:s:S:r:b:r:t:f:Bn", long_options, &option_index)) != -1) {
		switch(op) {
			case 1: // this means double argument
				if (last_op == 'c') {
					trans_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				} else if (last_op == 'S') {
					trans_attr.addr.sa_in.sin_port = htons(atoi(optarg));
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
				trans_attr.debug = trans_attr.debug * 2 + 1;
				break;
			case 'c':
				trans_attr.server = 0;
				host = gethostbyname(optarg);
				//FIXME: if (host->h_addrtype == AF_INET6)
				memcpy(&trans_attr.addr.sa_in.sin_addr, host->h_addr_list[0], 4);
				break;
			case 's':
				trans_attr.server = 10;
				inet_pton(AF_INET, "0.0.0.0", &trans_attr.addr.sa_in.sin_addr);
				trans_attr.addr.sa_in.sin_port = htons(atoi(optarg));
				break;
			case 'S':
				trans_attr.server = 10;
				host = gethostbyname(optarg);
				//FIXME: if (host->h_addrtype == AF_INET6)
				memcpy(&trans_attr.addr.sa_in.sin_addr, host->h_addr_list[0], 4);
				break;
			case 'q':
				trans_attr.debug = 0;
				break;
			case 'f':
				pcap_file = optarg;
				break;
			case 'B':
				banner = 1;
				break;
			case 'n':
				priv.docheck = 0;
				break;
			case 'b':
				block_size = strtoul(optarg, &tmp_s, 0);
				if (errno || block_size == 0) {
					ERROR_LOG("Invalid block size, assuming default (%u)", DEFAULT_BLOCK_SIZE);
					break;
				}
				if (tmp_s[0] != 0) {
					set_size(&block_size, tmp_s);
				}
				INFO_LOG(trans_attr.debug > 1, "block size: %u", block_size);
				break;
			case 'r':
				recv_num = strtoul(optarg, NULL, 0);

				if (errno || recv_num == 0)
					ERROR_LOG("Invalid recv_num, assuming default (%u)", DEFAULT_RECV_NUM);
				break;
			default:
				ERROR_LOG("Failed to parse arguments");
				print_help(argv);
				exit(EINVAL);
		}
		last_op = op;
	}

	if (trans_attr.server == -1) {
		ERROR_LOG("Must be either client or server!");
		print_help(argv);
		exit(EINVAL);
	}

	if (block_size == 0)
		block_size = DEFAULT_BLOCK_SIZE;
	if (recv_num == 0)
		recv_num = DEFAULT_RECV_NUM;

	trans_attr.rq_depth = recv_num+1;
	trans_attr.sq_depth = recv_num+1;

	/* msk init */
	TEST_Z(msk_init(&trans, &trans_attr));

	if (!trans) {
		ERROR_LOG("msk_init failed! panic!");
		exit(-1);
	}

	/* open pcap file */
	pcap = pcap_open_offline( pcap_file, errbuf );

	if (pcap == NULL) {
		ERROR_LOG("Couldn't open pcap file: %s", errbuf);
		return EINVAL;
	}

	/* finish msk init */
	const size_t mr_size = (recv_num+1)*block_size;

	if (trans_attr.server == 0)
		TEST_Z(msk_connect(trans));
	else {
		listen_trans = trans;
		TEST_Z(msk_bind_server(listen_trans));
		trans = msk_accept_one(listen_trans);
	}

	TEST_NZ(rdmabuf = malloc(mr_size));
	memset(rdmabuf, 0, mr_size);

	TEST_NZ(mr = msk_reg_mr(trans, rdmabuf, mr_size, IBV_ACCESS_LOCAL_WRITE));

	TEST_NZ(data = malloc((recv_num+1)*sizeof(msk_data_t)));

	for (i=0; i < recv_num + 1; i++) {
		data[i].data = rdmabuf+i*block_size;
		data[i].max_size = block_size;
		data[i].mr = mr;
	}
	wdata = &data[recv_num];

	trans->private_data = &priv;

	pthread_mutex_init(&priv.lock, NULL);
	pthread_cond_init(&priv.cond, NULL);

	for (i=0; i<recv_num; i++) {
		TEST_Z(msk_post_recv(trans, &data[i], callback_recv, callback_error, NULL));
	}

	pthread_mutex_lock(&priv.lock);

	if (trans->server == 0)
		TEST_Z(msk_finalize_connect(trans));
	else
		TEST_Z(msk_finalize_accept(trans));

	/* set on first packet */
	uint32_t send_ip = 0;
	uint32_t recv_ip = 0;
	uint16_t send_port = 0;
	uint16_t recv_port = 0;

	i=0;
	while ((rc = pcap_next_ex(pcap, &priv.pcaphdr, (const u_char**)&priv.packet)) >= 0) {
		INFO_LOG(trans->debug & (MSK_DEBUG_SEND|MSK_DEBUG_RECV), "Iteration %d", i++);

		/* first packet: */
		if (send_ip == 0) {
			/* who talks first? */
			if ((trans->server == 0 && banner == 0) || (trans->server && banner == 1)) {
				send_ip = priv.packet->ipv6.ip_src.s6_addr32[3];
				send_port = priv.packet->tcp.th_sport;
				recv_ip = priv.packet->ipv6.ip_dst.s6_addr32[3];
				recv_port = priv.packet->tcp.th_dport;
			} else {
				send_ip = priv.packet->ipv6.ip_dst.s6_addr32[3];
				send_port = priv.packet->tcp.th_dport;
				recv_ip = priv.packet->ipv6.ip_src.s6_addr32[3];
				recv_port = priv.packet->tcp.th_sport;
			}
		}

		/* all packets: decide if we send it or if we wait till we receive another */
		if (priv.packet->ipv6.ip_src.s6_addr32[3] == send_ip &&
		    priv.packet->tcp.th_sport == send_port) {
			if (priv.pcaphdr->len != priv.pcaphdr->caplen) {
				ERROR_LOG("Can't send truncated data! make sure you've stored all the capture (-t in rmitm)");
				rc = EINVAL;
				break;
			}
			memcpy(wdata->data, priv.packet->data, priv.pcaphdr->len);
			wdata->size = priv.pcaphdr->len;
			rc = msk_post_send(trans, wdata, callback_send, callback_error, NULL);
			if (rc) {
				ERROR_LOG("msk_post_send failed with rc %d (%s)", rc, strerror(rc));
				break;
			}
		} else if (priv.packet->ipv6.ip_src.s6_addr32[3] == recv_ip &&
		    priv.packet->tcp.th_sport == recv_port) {
			INFO_LOG(trans->debug & (MSK_DEBUG_SEND|MSK_DEBUG_RECV), "Waiting");
			pthread_cond_wait(&priv.cond, &priv.lock);
			if (priv.rc != 0) {
				/* got an error in recv thread */
				ERROR_LOG("Stopping loop");
				rc = priv.rc;
				break;
			}
		} else {
			ERROR_LOG("Multiple streams in pcap file? Stopping loop.");
			break;
		}
	}
	pthread_mutex_unlock(&priv.lock);
	/* mooshika doesn't use negative return values, so hopefully -1 can only mean pcap error */
	if (rc == -1) {
		ERROR_LOG("Pcap error: %s", pcap_geterr(pcap));
	}

	pcap_close(pcap);
	msk_destroy_trans(&trans);

	/* -2 is pcap way of saying end of file */
	if (rc == -2) {
		rc = 0;
		printf("Replay ended succesfully!\n");
	}

	return rc;
}

