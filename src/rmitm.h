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
 *  UDP header
 *  User Data Protocol
 *  Static header size: 8 bytes
 */
struct udp_hdr
{
    u_int16_t uh_sport;       /* soure port */
    u_int16_t uh_dport;       /* destination port */
    u_int16_t uh_ulen;        /* length */
    u_int16_t uh_sum;         /* checksum */
};


#define ETHER_ADDR_LEN 0x6

/*
 *  Ethernet II header
 *  Static header size: 14 bytes
 */
struct ethernet_hdr
{
    u_int8_t  ether_dhost[ETHER_ADDR_LEN];/* destination ethernet address */
    u_int8_t  ether_shost[ETHER_ADDR_LEN];/* source ethernet address */
    u_int16_t ether_type;                 /* protocol */
};

struct pkt_hdr {
	struct ipv6_hdr ipv6;
	struct udp_hdr udp;
	uint8_t data[0];
};

#define PACKET_HDR_LEN sizeof(struct pkt_hdr)

#define	CHECKSUM_CARRY(x) \
    (~((x & 0xffff) + (x >> 16)) & 0xffff)

static uint16_t checksum(u_int16_t *data, int len) {
	uint32_t sum = 0;
	union {
		int16_t s;
		u_int8_t b[2];
	} pad;

	while (len > 1) {
		sum += *data++;
		len -= 2;
                if (sum >= 0x10000)
	                sum = (sum & 0xffff) + (sum >> 16);
	}

	if (len == 1) {
		pad.b[0] = *(u_int8_t *)data;
		pad.b[1] = 0;
		sum += pad.s;
	}

	return sum;
}

static void ipv6_udp_checksum(struct pkt_hdr *hdr) {
	uint32_t sum;
	hdr->udp.uh_sum = 0;

	sum  = checksum((uint16_t*)&hdr->ipv6.ip_src, 2*sizeof(struct in6_addr));
	sum += htons(IPPROTO_UDP) + hdr->udp.uh_ulen;
	sum += checksum((uint16_t*)&hdr->udp, ntohs(hdr->udp.uh_ulen));
	hdr->udp.uh_sum = CHECKSUM_CARRY(sum);

//	hdr->udp.uh_sum = checksum((uint16_t*)hdr, ntohs(hdr->ipv6.ip_len))
}

static inline int min(int a, int b) {
	return (a < b) ? a: b;
}

#endif /* _RMITM_H_ */
