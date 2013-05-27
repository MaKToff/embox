/**
 * @file
 * @brief This module implements the Address Resolution Protocol (ARP),
 * which is used to convert IP addresses into a low-level hardware address.
 * @details RFC 826
 *
 * @date 11.03.09
 * @author Anton Bondarev
 * @author Nikolay Korotky
 * @author Ilia Vaprol
 */

#include <arpa/inet.h>
#include <assert.h>
#include <embox/net/pack.h>
#include <errno.h>
#include <net/arp.h>
#include <net/arp_queue.h>
#include <net/if_arp.h>
#include <net/if_ether.h>
#include <net/if_packet.h>
#include <net/inetdevice.h>
#include <net/neighbour.h>
#include <net/route.h>
#include <net/skbuff.h>
#include <string.h>

EMBOX_NET_PACK(ETH_P_ARP, arp_rcv);

static int arp_build(struct sk_buff *skb, unsigned short oper,
		unsigned short paddr_space, unsigned char haddr_len,
		unsigned char paddr_len, const void *source_haddr,
		const void *source_paddr, const void *dest_haddr,
		const void *dest_paddr, const void *target_haddr,
		struct net_device *dev) {
	int ret;
	struct arpghdr *arph;
	struct arpg_stuff arph_stuff;

	assert(skb != NULL);
	assert((haddr_len != 0) && (paddr_len != 0));
	assert((source_paddr != NULL) && (dest_paddr != NULL));
	assert((dev != NULL) && (haddr_len == dev->addr_len));

	/* Get default arguments */
	source_haddr = source_haddr != NULL ? source_haddr : &dev->dev_addr[0];

	/* Setup some fields */
	skb->dev = dev;
	skb->protocol = ETH_P_ARP;
	skb->nh.raw = skb->mac.raw + ETH_HEADER_SIZE;

	/* Make device specific header */
	assert(dev->ops != NULL);
	assert(dev->ops->create_hdr != NULL);
	ret = dev->ops->create_hdr(skb, skb->protocol,
			target_haddr, source_haddr);
	if (ret != 0) {
		return ret;
	}

	/* Setup fixed-length fields */
	arph = skb->nh.arpgh;
	assert(arph != NULL);
	arph->ha_space = htons(dev->type);
	arph->pa_space = htons(paddr_space);
	arph->ha_len = haddr_len;
	arph->pa_len = paddr_len;
	arph->oper = htons(oper);

	/* Setup variable-length fields */
	arpg_make_stuff(arph, &arph_stuff);
	memcpy(arph_stuff.sha, source_haddr, haddr_len);
	memcpy(arph_stuff.spa, source_paddr, paddr_len);
	if (dest_haddr != NULL) {
		memcpy(arph_stuff.tha, dest_haddr, haddr_len);
	}
	else {
		memset(arph_stuff.tha, 0, haddr_len);
	}
	memcpy(arph_stuff.tpa, dest_paddr, paddr_len);

	return 0;
}

static int arp_xmit(struct sk_buff *skb) {
	/* fall through to dev layer */
	return dev_xmit_skb(skb);
}

int arp_send(unsigned short oper, unsigned short paddr_space,
		unsigned char haddr_len, unsigned char paddr_len,
		const void *source_haddr, const void *source_paddr,
		const void *dest_haddr, const void *dest_paddr,
		const void *target_haddr, struct net_device *dev) {
	int ret;
	struct sk_buff *skb;

	if ((haddr_len == 0) || (paddr_len == 0)
			|| (source_paddr == NULL) || (dest_paddr == NULL)
			|| (dev == NULL) || (haddr_len != dev->addr_len)) {
		return -EINVAL;
	}

	/* check device flags */
	if (dev->flags & IFF_NOARP) {
		return -EINVAL;
	}

	/* allocate net package */
	skb = skb_alloc(ETH_HEADER_SIZE + ARPG_CALC_HDR_SZ(haddr_len, paddr_len));
	if (skb == NULL) {
		return -ENOMEM;
	}

	/* build package */
	ret = arp_build(skb, oper, paddr_space, haddr_len, paddr_len, source_haddr,
			source_paddr, dest_haddr, dest_paddr, target_haddr, dev);
	if (ret != 0) {
		skb_free(skb);
		return ret;
	}

	/* FIXME */
	assert(ETH_HEADER_SIZE + ARPG_HEADER_SIZE(skb->nh.arpgh) == skb->len);

	/* and send */
	return arp_xmit(skb);
}

int arp_resolve(struct sk_buff *skb) {
	int ret;
	in_addr_t daddr;

	assert(skb != NULL);

	/* get ip after routing */
	ret = rt_fib_route_ip(skb->nh.iph->daddr, &daddr);
	if (ret != 0) {
		return ret;
	}

	/* loopback */
	if (ip_is_local(daddr, false, false)) {
		memset(skb->mac.ethh->h_dest, 0x00, ETH_ALEN);
		return ENOERR;
	}

	/* broadcast */
	if (daddr == htonl(INADDR_BROADCAST)) {
		memset(skb->mac.ethh->h_dest, 0xFF, ETH_ALEN);
		return ENOERR;
	}

	/* someone on the net */
	ret = neighbour_get_hardware_address((const unsigned char *)&daddr,
			sizeof daddr, skb->dev, sizeof skb->mac.ethh->h_dest,
			skb->mac.ethh->h_dest, NULL);
	if (ret != ENOERR) {
		return ret;
	}

	return ENOERR;
}

/**
 * receive ARP request, send ARP response
 */
static int arp_hnd_request(struct arpghdr *arph, struct arpg_stuff *arps,
		struct sk_buff *skb, struct net_device *dev) {
	int ret;
	unsigned char haddr_len, paddr_len;
	unsigned char src_paddr[MAX_ADDR_LEN];
	unsigned char dst_haddr[MAX_ADDR_LEN], dst_paddr[MAX_ADDR_LEN];
	struct in_device *in_dev;

	in_dev = inetdev_get_by_dev(dev);
	assert(in_dev != NULL);

	haddr_len = arph->ha_len;
	paddr_len = arph->pa_len;

	/* check protocol capabilities */
	if ((arph->pa_space != htons(ETH_P_IP))
			|| (paddr_len != sizeof in_dev->ifa_address)) {
		skb_free(skb);
		return 0; /* FIXME error: only IPv4 is supported */
	}

	/* check recipient */
	if (0 != memcmp(arps->tpa, &in_dev->ifa_address, paddr_len)) {
		skb_free(skb);
		return 0; /* error: not for us */
	}

	/* get source protocol address */
	memcpy(&src_paddr[0], arps->tpa, paddr_len);

	/* get dest addresses */
	memcpy(&dst_haddr[0], arps->sha, haddr_len);
	memcpy(&dst_paddr[0], arps->spa, paddr_len);

	/* build reply */
	ret = arp_build(skb, ARP_OPER_REPLY, ntohs(arph->pa_space), haddr_len,
			paddr_len, NULL, &src_paddr[0], &dst_haddr[0], &dst_paddr[0],
			&dst_haddr[0], dev);
	if (ret != 0) {
		skb_free(skb);
		return ret;
	}

	/* and send */
	return arp_xmit(skb);
}

/**
 * receive ARP response, update neighbours
 */
static int arp_hnd_reply(struct arpghdr *arph, struct arpg_stuff *arps,
		struct sk_buff *skb, struct net_device *dev) {
	int ret;

	assert(arph != NULL);
	assert(arps != NULL);

	/* save destination hardware and protocol addresses */
	ret = neighbour_add(arps->sha, arph->ha_len, arps->spa, arph->pa_len,
			dev, 0);
	arp_queue_process(skb);
	skb_free(skb);
	return ret;
}

/**
 * Process an arp request.
 */
static int arp_process(struct sk_buff *skb, struct net_device *dev) {
	struct arpghdr *arph;
	struct arpg_stuff arph_stuff;

	assert(skb != NULL);
	assert(dev != NULL);

	arph = skb->nh.arpgh;
	assert(arph != NULL);

	/* check hardware and protocol address lengths */
	if ((skb->nh.raw - skb->mac.raw) + ARPG_HEADER_SIZE(arph) > skb->len) {
		skb_free(skb);
		return 0; /* error: bad packet */
	}

	/* check device capabilities */
	if ((arph->ha_space != htons(dev->type))
			|| (arph->ha_len != dev->addr_len)) {
		skb_free(skb);
		return 0; /* error: invalid hardware address info */
	}

	arpg_make_stuff(arph, &arph_stuff);

	/* process the packet by the operation code */
	switch (ntohs(arph->oper)) {
	default:
		skb_free(skb);
		return 0; /* error: bad operation type */
	case ARP_OPER_REQUEST:
		/* handling request */
		return arp_hnd_request(arph, &arph_stuff, skb, dev);
	case ARP_OPER_REPLY:
		/* handling reply */
		return arp_hnd_reply(arph, &arph_stuff, skb, dev);
	}
}

static int arp_rcv(struct sk_buff *skb, struct net_device *dev) {
	assert(skb != NULL);
	assert(dev != NULL);

	/* check recipient */
	switch (pkt_type(skb)) {
	default:
		break; /* error: not for us */
	case PACKET_HOST:
	case PACKET_BROADCAST:
	case PACKET_MULTICAST:
		/* check device flags */
		if (dev->flags & IFF_NOARP) {
			break; /* error: arp doesn't supported */
		}
		/* handle package */
		return arp_process(skb, dev);
	}

	/* pretend that it was not */
	skb_free(skb);
	return 0;
}