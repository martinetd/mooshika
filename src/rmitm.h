/* Based on tcpreplay's tcpr.h, itself based from libnet's libnet-headers.h. Thanks. */

#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>

#ifndef _RMITM_H_
#define _RMITM_H_

#if 0
struct in6_addr
{
    union
    {
        u_int8_t   __u6_addr8[16];
        u_int16_t  __u6_addr16[8];
        u_int32_t  __u6_addr32[4];
    } __u6_addr;            /* 128-bit IP6 address */
};
#define s6_addr __u6_addr.__u6_addr8
#define s6_addr8 __u6_addr.__u6_addr8
#define s6_addr16 __u6_addr.__u6_addr16
#define s6_addr32 __u6_addr.__u6_addr32
#endif

/*
 *  IPv6 header
 *  Internet Protocol, version 6
 *  Static header size: 40 bytes
 */
struct ipv6_hdr
{
    u_int8_t ip_flags[4];     /* version, traffic class, flow label */
    u_int16_t ip_len;         /* total length */
    u_int8_t ip_nh;           /* next header */
    u_int8_t ip_hl;           /* hop limit */
    struct in6_addr ip_src, ip_dst; /* source and dest address */
};

/*
 *  tcp header structure. This is the minimal header (20 bytes)
 */ 

typedef struct tcp_hdr {
	u_int16_t th_sport;
	u_int16_t th_dport;
	u_int32_t th_seq_nr;
	u_int32_t th_ack_nr;
	u_int8_t th_data_off;
	u_int8_t th_flags;
	u_int16_t th_window;
	u_int16_t th_sum;
	u_int16_t th_urgptr;
} tcp_hdr_t;

#define TH_DO_MASK      0xf0

#define TH_FLAGS_MASK   0x3f
#define THF_FIN         0x1
#define THF_SYN         0x2
#define THF_RST         0x4
#define THF_PSH         0x8
#define THF_ACK         0x10
#define THF_URG         0x20

struct pkt_hdr {
	struct ipv6_hdr ipv6;
	struct tcp_hdr tcp;
	uint8_t data[0];
};

#define PACKET_HDR_LEN sizeof(struct pkt_hdr)

#define	CHECKSUM_CARRY(x) do {          \
	x = ((x & 0xffff) + (x >> 16)); \
	if (x > 0xffff)                 \
		x -= 0xffff;            \
} while (0)

static inline uint16_t checksum(u_int16_t *data, int len) {
	uint32_t sum = 0;
	union {
		int16_t s;
		u_int8_t b[2];
	} pad;

	while (len > 1) {
		sum += *data++;
		len -= 2;
		if (sum >= 0x10000)
			sum -= 0xffff;
	}

	if (len == 1) {
		pad.b[0] = *(u_int8_t *)data;
		pad.b[1] = 0;
		sum += pad.s;
	}

	CHECKSUM_CARRY(sum);
	return sum;
}

static inline void ipv6_tcp_checksum(struct pkt_hdr *hdr) {
	uint32_t sum;
	hdr->tcp.th_sum = 0;

	sum  = checksum((uint16_t*)&hdr->ipv6.ip_src, 2*sizeof(struct in6_addr));
	sum += htons(IPPROTO_TCP + ntohs(hdr->ipv6.ip_len));
	sum += checksum((uint16_t*)&hdr->tcp, ntohs(hdr->ipv6.ip_len));

	CHECKSUM_CARRY(sum);
	hdr->tcp.th_sum = ((~sum) & 0xffff);
}

static inline int min(int a, int b) {
	return (a < b) ? a: b;
}
static inline int max(int a, int b) {
	return (a < b) ? b: a;
}

#endif /* _RMITM_H_ */
