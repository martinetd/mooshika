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

#include "mooshika.h"
#include "../utils.h"
#include "../rmitm.h"

#define DEFAULT_BLOCK_SIZE 1024*1024 // nfs page size
#define DEFAULT_HARD_TRUNC_LEN (64*1024-1)
#define PACKET_SIZE (64*1024-1)

struct thread_arg {
	char *pcap_filename;
	uint32_t block_size;
};

static void print_help(char **argv) {
	printf("Usage: %s -s port -c addr port [-f pcap.out]\n", argv[0]);
	printf("Mandatory arguments (these two don't work atm - assumes localhost:2049):\n"
		"	-c, --client addr port: \n"
		"	-s, --server addr port: \n"
		"Optional arguments:\n"
		"	-f, --file pcap.out: output file (defaults to stdout)\n"
		"	-r, --raw: assume input is binary, not hex\n"
		"	-b, --block-size size: size of packets to send (default: %u)\n"
		"	-v, --verbose: verbose, more v for more verbosity\n"
		"	-q, --quiet: quiet output\n"
		"	--not-an-option-yet: disable appending the tcp rpc header...\n",
		DEFAULT_BLOCK_SIZE);

}

int main(int argc, char **argv) {
	msk_trans_attr_t s_attr;
	msk_trans_attr_t c_attr;
	int hex;

	pcap_t *pcap;
	pcap_dumper_t *pcap_dumper;
	struct pcap_pkthdr pcaphdr;
	char data[PACKET_SIZE];
	struct pkt_hdr *pkt_hdr = (struct pkt_hdr *)data;
	uint32_t *rpchdr = (uint32_t*)(data + PACKET_HDR_LEN);
	char *buffer = data + PACKET_HDR_LEN + 4;
	int nread;

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
		{ "file",	required_argument,	0,		'f' },
		{ "raw",	no_argument,		0,		'r' },
		{ 0,		0,			0,		 0  }
	};


	memset(&s_attr, 0, sizeof(msk_trans_attr_t));
	memset(&c_attr, 0, sizeof(msk_trans_attr_t));
	memset(&thread_arg, 0, sizeof(thread_arg));

	s_attr.node = NULL;
	s_attr.port = NULL;
	c_attr.node = NULL;
	c_attr.port = NULL;
	c_attr.debug = 1;
	hex = 1;
	thread_arg.pcap_filename = "-";

	last_op = 0;
	while ((op = getopt_long(argc, argv, "-@hvqs:c:w:b:f:r", long_options, &option_index)) != -1) {
		switch(op) {
			case 1: // this means double argument
				if (last_op == 'c') {
					c_attr.port = optarg;
				} else if (last_op == 's') {
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
				c_attr.node = optarg;
				break;
			case 's':
				s_attr.node = "::";
				s_attr.port = optarg;
				break;
			case 'S':
				s_attr.node = optarg;
				break;
			case 'w':
				ERROR_LOG("-w has become deprecated, use -f or --file now. Proceeding anyway");
				/* fallthrough */
			case 'f':
				thread_arg.pcap_filename = optarg;
				break;
			case 'r':
				hex = 0;
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


	if (thread_arg.block_size == 0)
		thread_arg.block_size = DEFAULT_BLOCK_SIZE;

	pcap = pcap_open_dead(DLT_RAW, PACKET_SIZE);
	TEST_NZ(pcap_dumper = pcap_dump_open(pcap, thread_arg.pcap_filename));

	memset(pkt_hdr, 0, sizeof(*pkt_hdr));

	pkt_hdr->ipv6.ip_flags[0] = 0x60; /* 6 in the leftmosts 4 bits */
	pkt_hdr->ipv6.ip_nh = IPPROTO_TCP;
	pkt_hdr->ipv6.ip_hl = 1;
	/** @todo: add options, use one of :
			CLIENT
		child_trans->cm_id->route.addr.dst_sin
		child_trans->cm_id->route.addr.src_sin
			RMITM
		c_trans->cm_id->route.addr.src_sin
		c_trans->cm_id->route.addr.dst_sin
			SERVER
	*/
	pkt_hdr->ipv6.ip_src.s6_addr16[4] = 0; /*0xffff; */
	pkt_hdr->ipv6.ip_src.s6_addr32[3] = ntohl(1); /* get ipv4 here */
	pkt_hdr->tcp.th_sport = ntohs(2049);

	pkt_hdr->ipv6.ip_dst.s6_addr16[4] = 0; /* 0xffff; */
	pkt_hdr->ipv6.ip_dst.s6_addr32[3] = ntohl(1); /* get ipv4 here */
	pkt_hdr->tcp.th_dport = ntohs(2049);

	pkt_hdr->tcp.th_data_off = INT8_C(sizeof(struct tcp_hdr) * 4); /* *4 because words of 2 bits? it's odd. */
	pkt_hdr->tcp.th_window = htons(100);
	/* assume stream - nothing to ack 
	 * pkt_hdr->tcp.th_flags = THF_ACK;
	 * pkt_hdr->tcp.th_ack_nr = 0;
	 */

	while (!feof(stdin)) {
		if (!hex) {
			nread = fread(buffer, 1, PACKET_SIZE - PACKET_HDR_LEN - 4, stdin);
			if (nread == 0)
				break;
		} else {

			/* fugly code to parse hexa dump, allowing truncated input.
			 * valid format are xxd or hexdump (with or without -C)
			 * should be groups of 1 or 2 bytes */
			char line[80], *cur;
			int no_double_space;

			buffer = data + PACKET_HDR_LEN + 4;
			/* stop on eof or empty line */
			while (fgets(line, 80, stdin) && strcmp(line, "\n") != 0) {
				/* skip first word iff longer than 4 chars */
				cur = strchr(line, ' ');

				if (!cur)
					break; /* not a valid line?! */

				if (cur - line > 4) {
					while (cur[0] == ' ')
						cur += 1;
				} else
					cur = line;

				/* check if we should allow double-spaces */
				if (cur[2] == ' ' && cur[5] == ' ')
					no_double_space = 0;
				else
					no_double_space = 1;

				while ((nread = sscanf(cur, "%2hhx%[^|\n]", buffer, cur)) > 0) {
					buffer++;

					if (nread != 2)
						break;

					if (no_double_space && cur[0] == ' ' && cur[1] == ' ')
						break;
				}
			}
			nread = buffer - (data + PACKET_HDR_LEN + 4);
			buffer[0] = '\0';
			buffer = data + PACKET_HDR_LEN + 4;
		}

		*rpchdr = htonl(0x80000000 + nread);
		gettimeofday(&pcaphdr.ts, NULL);
		pcaphdr.len = min(nread + PACKET_HDR_LEN + 4, PACKET_SIZE);
		pcaphdr.caplen = pcaphdr.len;


		pkt_hdr->ipv6.ip_len = htons(nread + 4 + sizeof(struct tcp_hdr));

		ipv6_tcp_checksum(pkt_hdr);

		pcap_dump((u_char*)pcap_dumper, &pcaphdr, (u_char*)pkt_hdr);

		pkt_hdr->tcp.th_seq_nr = htonl(ntohl(pkt_hdr->tcp.th_seq_nr) + ntohs(pkt_hdr->ipv6.ip_len) - sizeof(struct tcp_hdr));

                pcap_dump_flush(pcap_dumper);
	}

	pcap_dump_close(pcap_dumper);

	pcap_close(pcap);

	return 0;
}

