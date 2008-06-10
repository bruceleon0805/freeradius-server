/*
 * dhcp.c	Functions to send/receive dhcp packets.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2008 The FreeRADIUS server project
 * Copyright 2008 Alan DeKok <aland@deployingradius.com>
 */

#include	<freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/libradius.h>
#include <freeradius-devel/udpfromto.h>
#include <freeradius-devel/dhcp.h>

#ifdef WITH_DHCP
#define DHCP_CHADDR_LEN	(16)
#define DHCP_SNAME_LEN	(64)
#define DHCP_FILE_LEN	(128)
#define DHCP_VEND_LEN	(308)
#define DHCP_OPTION_MAGIC_NUMBER (0x63825363)

typedef struct dhcp_packet_t {
	uint8_t		opcode;
	uint8_t		htype;
	uint8_t		hlen;
	uint8_t		hops;
	uint32_t	xid;	/* 4 */
	uint16_t	secs;	/* 8 */
	uint16_t	flags;
	uint32_t	ciaddr;	/* 12 */
	uint32_t	yiaddr;	/* 16 */
	uint32_t	siaddr;	/* 20 */
	uint32_t	giaddr;	/* 24 */
	uint8_t		chaddr[DHCP_CHADDR_LEN]; /* 28 */
	char		sname[DHCP_SNAME_LEN]; /* 44 */
	char		file[DHCP_FILE_LEN]; /* 108 */
	uint32_t	option_format; /* 236 */
	uint8_t		options[DHCP_VEND_LEN];
} dhcp_packet_t;

/*
 *	INADDR_ANY : 68 -> INADDR_BROADCAST : 67	DISCOVER
 *	INADDR_BROADCAST : 68 <- SERVER_IP : 67		OFFER
 *	INADDR_ANY : 68 -> INADDR_BROADCAST : 67	REQUEST
 *	INADDR_BROADCAST : 68 <- SERVER_IP : 67		ACK
 */
static const char *dhcp_header_names[] = {
	"DHCP-Opcode",
	"DHCP-Hardware-Type",
	"DHCP-Hardware-Address-Length",
	"DHCP-Hop-Count",
	"DHCP-Transaction-Id",
	"DHCP-Number-of-Seconds",
	"DHCP-Flags",
	"DHCP-Client-IP-Address",
	"DHCP-Your-IP-Address",
	"DHCP-Server-IP-Address",
	"DHCP-Gateway-IP-Address",
	"DHCP-Client-Hardware-Address",
	"DHCP-Server-Host-Name",
	"DHCP-Boot-Filename",

	NULL
};

static const char *dhcp_message_types[] = {
	"invalid",
	"DHCP-Discover",
	"DHCP-Offer",
	"DHCP-Request",
	"DHCP-Decline",
	"DHCP-Ack",
	"DHCP-NAK",
	"DHCP-Release",
	"DHCP-Inform",
	"DHCP-Force-Renew",
};

static int dhcp_header_sizes[] = {
	1, 1, 1, 1,
	4, 2, 2, 4,
	4, 4, 4,
	DHCP_CHADDR_LEN,
	DHCP_SNAME_LEN,
	DHCP_FILE_LEN
};


/*
 *	Some clients silently ignore responses less than 300 bytes.
 */
#define MIN_PACKET_SIZE (244)
#define DEFAULT_PACKET_SIZE (576)
#define MAX_PACKET_SIZE (1500 - 40)

/*
 *	DHCPv4 is only for IPv4.  Broadcast only works if udpfromto is
 *	defined.
 */
RADIUS_PACKET *fr_dhcp_recv(int sockfd)
{
	uint32_t		magic;
	struct sockaddr_storage	src;
	struct sockaddr_storage	dst;
	socklen_t		sizeof_src;
	socklen_t	        sizeof_dst;
	RADIUS_PACKET		*packet;

	packet = rad_alloc(0);
	if (!packet) return NULL;
	memset(packet, 0, sizeof(packet));

	packet->data = malloc(MAX_PACKET_SIZE);
	if (!packet->data) {
		rad_free(&packet);
		return NULL;
	}

	packet->sockfd = sockfd;
	packet->data_len = recvfrom(sockfd, packet->data, MAX_PACKET_SIZE, 0,
				    (struct sockaddr *)&src, &sizeof_src);
	if (packet->data_len <= 0) {
		fprintf(stderr, "Failed reading DHCP socket: %s", strerror(errno));
		rad_free(&packet);
		return NULL;
	}

	if (packet->data_len < MIN_PACKET_SIZE) {
		fprintf(stderr, "DHCP packet is too small (%d < %d)",
		      packet->data_len, MIN_PACKET_SIZE);
		rad_free(&packet);
		return NULL;
	}

	if (packet->data[0] != 1) {
		fprintf(stderr, "Cannot receive DHCP server messages");
		rad_free(&packet);
		return NULL;
	}

	if (packet->data[1] != 1) {
		fprintf(stderr, "DHCP can only receive ethernet requests, not type %02x",
		      packet->data[1]);
		rad_free(&packet);
		return NULL;
	}

	if (packet->data[2] != 6) {
		fprintf(stderr, "Ethernet HW length is wrong length %d\n",
			packet->data[2]);
		rad_free(&packet);
		return NULL;
	}

	memcpy(&magic, packet->data + 236, 4);
	magic = ntohl(magic);
	if (magic != DHCP_OPTION_MAGIC_NUMBER) {
		fprintf(stderr, "Cannot do BOOTP\n");
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	Create unique keys for the packet.
	 */
	memcpy(&magic, packet->data + 4, 4);
	packet->id = ntohl(magic);

	/*
	 *	Check that it's a known packet type.
	 */
	if ((packet->data[240] != 53) ||
	    (packet->data[241] != 1) ||
	    (packet->data[242] == 0) ||
	    (packet->data[242] >= 8)) {
		fprintf(stderr, "Unknown, or badly formatted DHCP packet\n");
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	Create a unique vector from the MAC address and the
	 *	DHCP opcode.  This is a hack for the RADIUS
	 *	infrastructure in the rest of the server.
	 *
	 *	Note: packet->data[2] == 6, which is smaller than
	 *	sizeof(packet->vector)
	 *
	 *	FIXME:  Look for client-identifier in packet,
	 *      and use that, too?
	 */
	memset(packet->vector, 0, sizeof(packet->vector));
	memcpy(packet->vector, packet->data + 28, packet->data[2]);
	packet->vector[packet->data[2]] = packet->data[242];

	/*
	 *	FIXME: for DISCOVER / REQUEST: src_port == dst_port + 1
	 *	FIXME: for OFFER / ACK       : src_port = dst_port - 1
	 */

	packet->code = PW_DHCP_OFFSET | packet->data[242];

	/*
	 *	Unique keys are xid, client mac, and client ID?
	 */

	/*
	 *	FIXME: More checks, like DHCP packet type?
	 */


	{
		int port;
		struct sockaddr_storage si;
		socklen_t si_len = sizeof(si);

		/*
		 *	This should never fail...
		 */
		getsockname(sockfd, (struct sockaddr *) &si, &si_len);
		fr_sockaddr2ipaddr(&si, si_len, &packet->src_ipaddr, &port);
		packet->src_port = port;
	}

	if (librad_debug > 1) {
		char type_buf[64];
		const char *name = type_buf;
		char src_ip_buf[256], dst_ip_buf[256];
		
		if ((packet->code >= PW_DHCP_DISCOVER) &&
		    (packet->code <= PW_DHCP_INFORM)) {
			name = dhcp_message_types[packet->code - PW_DHCP_OFFSET];
		} else {
			snprintf(type_buf, sizeof(type_buf), "%d",
				 packet->code - PW_DHCP_OFFSET);
		}

		printf("Received %s of id %u from %s:%d to %s:%d\n",
		       name, (unsigned int) packet->id,
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 src_ip_buf, sizeof(src_ip_buf)),
		       packet->src_port,
		       inet_ntop(packet->dst_ipaddr.af,
				 &packet->dst_ipaddr.ipaddr,
				 dst_ip_buf, sizeof(dst_ip_buf)),
		       packet->dst_port);
		fflush(stdout);
	}

	return packet;
}


/*
 *	Send a DHCP packet.
 */
int fr_dhcp_send(RADIUS_PACKET *packet)
{
	struct sockaddr_storage	dst;
	struct sockaddr_storage	src;
	socklen_t		sizeof_dst;
	socklen_t		sizeof_src;

	fr_ipaddr2sockaddr(&packet->dst_ipaddr, packet->dst_port,
			   &dst, &sizeof_dst);

	/*
	 *	Currently unused...
	 */
	fr_ipaddr2sockaddr(&packet->src_ipaddr, packet->src_port,
			   &src, &sizeof_src);

	/*
	 *	Assume that the packet is encoded before sending it.
	 */
	return sendto(packet->sockfd, packet->data, packet->data_len, 0,
		      (struct sockaddr *)&dst, sizeof_dst);
}


int fr_dhcp_decode(RADIUS_PACKET *packet)
{
	int i;
	ssize_t total;
	uint8_t *p;
	uint32_t giaddr;
	VALUE_PAIR *head, *vp, **tail;
	VALUE_PAIR *maxms, *mtu;
	char buffer[2048];

	head = NULL;
	tail = &head;
	p = packet->data;
	
	if ((librad_debug > 2) && fr_log_fp) {
		for (i = 0; i < packet->data_len; i++) {
			if ((i & 0x0f) == 0x00) fprintf(stderr, "%d: ", i);
			fprintf(fr_log_fp, "%02x ", packet->data[i]);
			if ((i & 0x0f) == 0x0f) fprintf(fr_log_fp, "\n");
		}
		fprintf(fr_log_fp, "\n");
	}

	if (packet->data[1] != 1) {
		fprintf(stderr, "Packet is not Ethernet: %u\n",
		      packet->data[1]);
		return -1;
	}

	/*
	 *	Decode the header.
	 */
	for (i = 0; i < 14; i++) {
		vp = pairmake(dhcp_header_names[i], NULL, T_OP_EQ);
		if (!vp) {
			fprintf(stderr, "Parse error %s\n", librad_errstr);
			pairfree(&head);
			return -1;
		}


		if ((i == 11) && 
		    (packet->data[1] == 1) &&
		    (packet->data[2] == 6)) {
			vp->type = PW_TYPE_ETHERNET;
		}

		switch (vp->type) {
		case PW_TYPE_BYTE:
			vp->vp_integer = p[0];
			vp->length = 1;
			break;
			
		case PW_TYPE_SHORT:
			vp->vp_integer = (p[0] << 8) | p[1];
			vp->length = 2;
			break;
			
		case PW_TYPE_INTEGER:
			memcpy(&vp->vp_integer, p, 4);
			vp->vp_integer = ntohl(vp->vp_integer);
			vp->length = 4;
			break;
			
		case PW_TYPE_IPADDR:
			memcpy(&vp->vp_ipaddr, p, 4);
			vp->length = 4;
			break;
			
		case PW_TYPE_STRING:
			memcpy(vp->vp_strvalue, p, dhcp_header_sizes[i]);
			vp->vp_strvalue[dhcp_header_sizes[i]] = '\0';
			vp->length = strlen(vp->vp_strvalue);
			if (vp->length == 0) {
				pairfree(&vp);
			}
			break;
			
		case PW_TYPE_OCTETS:
			memcpy(vp->vp_octets, p, packet->data[2]);
			vp->length = packet->data[2];
			break;
			
		case PW_TYPE_ETHERNET:
			memcpy(vp->vp_ether, p, sizeof(vp->vp_ether));
			vp->length = sizeof(vp->vp_ether);
			break;
			
		default:
			fprintf(stderr, "BAD TYPE %d\n", vp->type);
			pairfree(&vp);
			break;
		}
		p += dhcp_header_sizes[i];

		if (!vp) continue;
		
		if (librad_debug > 1) {
			vp_prints(buffer, sizeof(buffer), vp);
			fprintf(stderr, "\t%s\n", buffer);
		}
		*tail = vp;
		tail = &vp->next;
	}
	
	/*
	 *	Loop over the options.
	 */
	p = packet->data + 240;
	total = packet->data_len - 240;

	while (total > 0) {
		int num_entries, alen;
		DICT_ATTR *da;

		if (*p == 0) break;
		if (*p == 255) break; /* end of options signifier */

		if (p[1] >= 253) {
			fprintf(stderr, "Attribute too long %u %u\n",
			      p[0], p[1]);
			goto do_next;
		}
				
		da = dict_attrbyvalue(DHCP2ATTR(p[0]));
		if (!da) {
			fprintf(stderr, "Attribute not in our dictionary: %u\n",
			      p[0]);
		do_next:
			total -= 2;
			total -= p[1];
			p += p[1];
			p += 2;
			continue;
		}

		vp = NULL;
		num_entries = 1;
		alen = p[1];
		p += 2;

		if (da->flags.array) {
			switch (da->type) {
			case PW_TYPE_BYTE:
				num_entries = alen;
				alen = 1;
				break;

			case PW_TYPE_SHORT:
				if ((alen & 0x01) != 0) goto raw;
				num_entries = alen / 2;
				alen = 2;
				break;

			case PW_TYPE_IPADDR:
			case PW_TYPE_INTEGER:
			case PW_TYPE_DATE:
				if ((alen & 0x03) != 0) goto raw;
				num_entries = alen / 4;
				alen = 4;
				break;

			default:
				break; /* really an internal sanity failure */
			}
		} else {
			num_entries = 1;

			switch (da->type) {
			case PW_TYPE_BYTE:
				if (alen != 1) goto raw;
				break;

			case PW_TYPE_SHORT:
				if (alen != 2) goto raw;
				break;

			case PW_TYPE_IPADDR:
			case PW_TYPE_INTEGER:
			case PW_TYPE_DATE:
				if (alen != 4) goto raw;
				break;

			default:
				break;
			}
		}

		for (i = 0; i < num_entries; i++) {
			vp = pairmake(da->name, NULL, T_OP_EQ);
			if (!vp) {
				fprintf(stderr, "Cannot build attribute %s\n",
					librad_errstr);
				pairfree(&head);
				return -1;
			}

			/*
			 *	Hacks for ease of use.
			 */
			if ((da->attr == DHCP2ATTR(0x3d)) &&
			    !da->flags.array &&
			    (alen == 7) && (*p == 1) && (num_entries == 1)) {
				vp->type = PW_TYPE_ETHERNET;
				memcpy(vp->vp_octets, p + 1, 6);
			} else

				switch (vp->type) {
				case PW_TYPE_BYTE:
					vp->vp_integer = p[0];
					break;
				
				case PW_TYPE_SHORT:
					vp->vp_integer = (p[0] << 8) | p[1];
					break;

				case PW_TYPE_INTEGER:
					memcpy(&vp->vp_integer, p, 4);
					vp->vp_integer = ntohl(vp->vp_integer);
					break;

				case PW_TYPE_IPADDR:
					memcpy(&vp->vp_ipaddr, p , 4);
					vp->length = 4;
					break;

				case PW_TYPE_STRING:
					memcpy(vp->vp_strvalue, p , alen);
					vp->vp_strvalue[alen] = '\0';
					break;

				raw:
					vp = pairmake(da->name, NULL, T_OP_EQ);
					if (!vp) {
						fprintf(stderr, "Cannot build attribute %s\n", librad_errstr);
						pairfree(&head);
						return -1;
					}

					vp->type = PW_TYPE_OCTETS;
				
				case PW_TYPE_OCTETS:
					memcpy(vp->vp_octets, p, alen);
					break;
				
				default:
					fprintf(stderr, "Internal sanity check %d %d\n", vp->type, __LINE__);
					pairfree(&vp);
					break;
				} /* switch over type */
				
			vp->length = alen;

			if (librad_debug > 1) {
				vp_prints(buffer, sizeof(buffer), vp);
				fprintf(stderr, "\t%s\n", buffer);
			}

			*tail = vp;
			tail = &vp->next;
			p += alen;
		} /* loop over array entries */
		
		total -= 2;
		total -= (alen * num_entries);
	}

	/*
	 *	If DHCP request, set ciaddr to zero.
	 */

	/*
	 *	Set broadcast flag for broken vendors, but only if
	 *	giaddr isn't set.
	 */
	memcpy(&giaddr, packet->data + 24, sizeof(giaddr));
	if (giaddr == htonl(INADDR_ANY)) {
		/*
		 *	DHCP Opcode is request
		 */
		vp = pairfind(head, DHCP2ATTR(256));
		if (vp && vp->lvalue == 3) {
			/*
			 *	Vendor is "MSFT 98"
			 */
			vp = pairfind(head, DHCP2ATTR(63));
			if (vp && (strcmp(vp->vp_strvalue, "MSFT 98") == 0)) {
				vp = pairfind(head, DHCP2ATTR(262));

				/*
				 *	Reply should be broadcast.
				 */
				if (vp) vp->lvalue |= 0x8000;
				packet->data[10] |= 0x80;			
			}
		}
	}

	/*
	 *	FIXME: Nuke attributes that aren't used in the normal
	 *	header for discover/requests.
	 */
	packet->vps = head;

	/*
	 *	Client can request a LARGER size, but not a smaller
	 *	one.  They also cannot request a size larger than MTU.
	 */
	maxms = pairfind(packet->vps, DHCP2ATTR(57));
	mtu = pairfind(packet->vps, DHCP2ATTR(26));

	if (mtu && (mtu->vp_integer < DEFAULT_PACKET_SIZE)) {
		fprintf(stderr, "DHCP Fatal: Client says MTU is smaller than minimum permitted by the specification.");
		return -1;
	}

	if (maxms && (maxms->vp_integer < DEFAULT_PACKET_SIZE)) {
		fprintf(stderr, "DHCP WARNING: Client says maximum message size is smaller than minimum permitted by the specification: fixing it");
		maxms->vp_integer = DEFAULT_PACKET_SIZE;
	}

	if (maxms && mtu && (maxms->vp_integer > mtu->vp_integer)) {
		fprintf(stderr, "DHCP WARNING: Client says MTU is smaller than maximum message size: fixing it");
		maxms->vp_integer = mtu->vp_integer;
	}

	if (librad_debug) fflush(stdout);

	return 0;
}


static int attr_cmp(const void *one, const void *two)
{
	const VALUE_PAIR * const *a = one;
	const VALUE_PAIR * const *b = two;

	/*
	 *	DHCP-Message-Type is first, for simplicity.
	 */
	if (((*a)->attribute == DHCP2ATTR(53)) &&
	    (*b)->attribute != DHCP2ATTR(53)) return -1;

	/*
	 *	Relay-Agent is last
	 */
	if (((*a)->attribute == DHCP2ATTR(82)) &&
	    (*b)->attribute != DHCP2ATTR(82)) return +1;

	return ((*a)->attribute - (*b)->attribute);
}


static size_t fr_dhcp_vp2attr(VALUE_PAIR *vp, uint8_t *p, size_t room)
{
	size_t length;
	uint32_t lvalue;

	/*
	 *	FIXME: Check room!
	 */
	room = room;		/* -Wunused */

	/*
	 *	Search for all attributes of the same
	 *	type, and pack them into the same
	 *	attribute.
	 */
	switch (vp->type) {
	case PW_TYPE_BYTE:
		length = 1;
		*p = vp->vp_integer & 0xff;
		break;
		
	case PW_TYPE_SHORT:
		length = 2;
		p[0] = (vp->vp_integer >> 8) & 0xff;
		p[1] = vp->vp_integer & 0xff;
		break;
		
	case PW_TYPE_INTEGER:
		length = 4;
		lvalue = htonl(vp->vp_integer);
		memcpy(p, &lvalue, 4);
		break;
		
	case PW_TYPE_IPADDR:
		length = 4;
		memcpy(p, &vp->vp_ipaddr, 4);
		break;
		
	case PW_TYPE_ETHERNET:
		length = 6;
		memcpy(p, &vp->vp_ether, 6);
		break;
		
	case PW_TYPE_STRING:
		memcpy(p, vp->vp_strvalue, vp->length);
		length = vp->length;
		break;
		
	case PW_TYPE_OCTETS:
		memcpy(p, vp->vp_octets, vp->length);
		length = vp->length;
		break;
		
	default:
		fprintf(stderr, "BAD TYPE2 %d\n", vp->type);
		length = 0;
		break;
	}

	return length;
}

int fr_dhcp_encode(RADIUS_PACKET *packet, RADIUS_PACKET *original)
{
	int i, num_vps;
	uint8_t *p;
	VALUE_PAIR *vp;
	uint32_t lvalue, mms;
	size_t dhcp_size, length;
	dhcp_packet_t *dhcp;
	char buffer[1024];

	if (packet->data) return 0;

	packet->data = malloc(MAX_PACKET_SIZE);
	if (!packet->data) return -1;

	packet->data_len = MAX_PACKET_SIZE;

	if (packet->code == 0) packet->code = PW_DHCP_NAK;

	if (librad_debug > 1) {
		char type_buf[64];
		const char *name = type_buf;
		char src_ip_buf[256], dst_ip_buf[256];
		
		if ((packet->code >= PW_DHCP_DISCOVER) &&
		    (packet->code <= PW_DHCP_INFORM)) {
			name = dhcp_message_types[packet->code - PW_DHCP_OFFSET];
		} else {
			snprintf(type_buf, sizeof(type_buf), "%d",
				 packet->code - PW_DHCP_OFFSET);
		}

		printf("Sending %s of id %u from %s:%d to %s:%d\n",
		       name, (unsigned int) packet->id,
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 src_ip_buf, sizeof(src_ip_buf)),
		       packet->src_port,
		       inet_ntop(packet->dst_ipaddr.af,
				 &packet->dst_ipaddr.ipaddr,
				 dst_ip_buf, sizeof(dst_ip_buf)),
		       packet->dst_port);
		fflush(stdout);
	}

	p = packet->data;

	mms = DEFAULT_PACKET_SIZE; /* maximum message size */

	/*
	 *	Client can request a LARGER size, but not a smaller
	 *	one.  They also cannot request a size larger than MTU.
	 */
	vp = pairfind(original->vps, DHCP2ATTR(57));
	if (vp && (vp->vp_integer > mms)) {
		mms = vp->vp_integer;
		
		if (mms > MAX_PACKET_SIZE) mms = MAX_PACKET_SIZE;
	}

	/*
	 *	RFC 3118: Authentication option.
	 */
	vp = pairfind(packet->vps, DHCP2ATTR(90));
	if (vp) {
		if (vp->length < 2) {
			memset(vp->vp_octets + vp->length, 0,
			       2 - vp->length);
			vp->length = 2;
		}

		if (vp->length < 3) {
			struct timeval tv;

			gettimeofday(&tv, NULL);
			vp->vp_octets[2] = 0;
			timeval2ntp(&tv, vp->vp_octets + 3);
			vp->length = 3 + 8;
		}

		/*
		 *	Configuration token (clear-text token)
		 */
		if (vp->vp_octets[0] == 0) {
			VALUE_PAIR *pass;
			vp->vp_octets[1] = 0;

			pass = pairfind(packet->vps, PW_CLEARTEXT_PASSWORD);
			if (pass) {
				length = pass->length;
				if ((length + 11) > sizeof(vp->vp_octets)) {
					length -= ((length + 11) - sizeof(vp->vp_octets));
				}
				memcpy(vp->vp_octets + 11, pass->vp_strvalue,
				       length);
				vp->length = length + 11;
			} else {
				vp->length = 11 + 8;
				memset(vp->vp_octets + 11, 8, 0);
				vp->length = 11 + 8;
			}
		} else {	/* we don't support this type! */
			fprintf(stderr, "DHCP-Authentication %d unsupported\n",
				vp->vp_octets[0]);
		}
	}

	if (!original) {
		*p++ = 1;	/* client message */
	} else {
		*p++ = 2;	/* server message */
	}
	*p++ = 1;		/* hardware type = ethernet */
	*p++ = original->data[2];
	*p++ = 0;		/* hops */

	if (!original) {	/* Xid */
		lvalue = fr_rand();
		memcpy(p, &lvalue, 4);
	} else {
		memcpy(p, original->data + 4, 4);
	}
	p += 4;

	memset(p, 0, 2);	/* secs are zero */
	p += 2;

	memcpy(p, original->data + 10, 6); /* copy flags && ciaddr */
	p += 6;

	/*
	 *	Set client IP address.
	 */
	vp = pairfind(packet->vps, DHCP2ATTR(264)); /* Your IP address */
	if (vp) {
		lvalue = vp->vp_ipaddr;
	} else {
		lvalue = htonl(INADDR_ANY);
	}
	memcpy(p, &lvalue, 4);	/* your IP address */
	p += 4;

	memset(p, 0, 4);	/* siaddr is zero */
	p += 4;

	memset(p, 0, 4);	/* gateway address is zero */
	p += 4;

	/*
	 *	FIXME: allow it to send client packets.
	 */
	if (!original) {
		librad_log("Need original to send response!");
		return -1;
	}

	memcpy(p, original->data + 28, DHCP_CHADDR_LEN);
	p += DHCP_CHADDR_LEN;

	memset(p, 0, 192);	/* bootp legacy */
	p += 192;

	lvalue = htonl(DHCP_OPTION_MAGIC_NUMBER); /* DHCP magic number */
	memcpy(p, &lvalue, 4);
	p += 4;

	/*
	 *	Print the header.
	 */
	if (librad_debug > 1) {
		uint8_t *pp = p;

		p = packet->data;

		for (i = 0; i < 14; i++) {
			vp = pairmake(dhcp_header_names[i], NULL, T_OP_EQ);
			if (!vp) {
				fprintf(stderr, "Parse error %s\n", librad_errstr);
				return -1;
			}
			
			switch (vp->type) {
			case PW_TYPE_BYTE:
				vp->vp_integer = p[0];
				vp->length = 1;
				break;
				
			case PW_TYPE_SHORT:
				vp->vp_integer = (p[0] << 8) | p[1];
				vp->length = 2;
				break;
				
			case PW_TYPE_INTEGER:
				memcpy(&vp->vp_integer, p, 4);
				vp->vp_integer = ntohl(vp->vp_integer);
				vp->length = 4;
				break;
				
			case PW_TYPE_IPADDR:
				memcpy(&vp->vp_ipaddr, p, 4);
				vp->length = 4;
				break;
				
			case PW_TYPE_STRING:
				memcpy(vp->vp_strvalue, p, dhcp_header_sizes[i]);
				vp->vp_strvalue[dhcp_header_sizes[i]] = '\0';
				vp->length = strlen(vp->vp_strvalue);
				break;
				
			case PW_TYPE_OCTETS: /* only for Client HW Address */
				memcpy(vp->vp_octets, p, packet->data[2]);
				vp->length = packet->data[2];
				break;
				
			case PW_TYPE_ETHERNET: /* only for Client HW Address */
				memcpy(vp->vp_ether, p, sizeof(vp->vp_ether));
				vp->length = sizeof(vp->vp_ether);
				break;
				
			default:
				fprintf(stderr, "Internal sanity check failed %d %d\n", vp->type, __LINE__);
				pairfree(&vp);
				break;
			}
			
			p += dhcp_header_sizes[i];
			
			vp_prints(buffer, sizeof(buffer), vp);
			fprintf(stderr, "\t%s\n", buffer);
			pairfree(&vp);
		}

		/*
		 *	Jump over DHCP magic number, response, etc.
		 */
		p = pp;
	}

	vp = pairfind(packet->vps, DHCP2ATTR(53));
	if (vp && (vp->vp_integer != (packet->code - PW_DHCP_OFFSET))) {
		fprintf(stderr, "Message-Type doesn't match! %d %d\n",
			packet->code, vp->vp_integer);
	}
	pairdelete(&packet->vps, DHCP2ATTR(0x35));

	/*
	 *	Before packing the attributes, re-order them so that
	 *	the array ones are all contiguous.  This simplifies
	 *	the later code.
	 */
	num_vps = 0;
	for (vp = packet->vps; vp != NULL; vp = vp->next) {
		num_vps++;
	}
	if (num_vps > 1) {
		VALUE_PAIR **array, **last;

		array = malloc(num_vps * sizeof(VALUE_PAIR *));
		
		i = 0;
		for (vp = packet->vps; vp != NULL; vp = vp->next) {
			array[i++] = vp;
		}
		
		/*
		 *	Sort the attributes.
		 */
		qsort(array, (size_t) num_vps, sizeof(VALUE_PAIR *),
		      attr_cmp);
		
		last = &packet->vps;
		for (i = 0; i < num_vps; i++) {
			*last = array[i];
			array[i]->next = NULL;
			last = &(array[i]->next);
		}
		free(array);
	}

	p[0] = 0x35;		/* DHCP-Message-Type */
	p[1] = 1;
	p[2] = packet->code - PW_DHCP_OFFSET;
	p += 3;

	/*
	 *	Pack in the attributes.
	 */
	vp = packet->vps;
	while (vp) {
		int num_entries = 1;
		
		VALUE_PAIR *same;
		uint8_t *plength, *pattr;

		if (!IS_DHCP_ATTR(vp)) goto next;
		if (((vp->attribute & 0xffff) > 255) &&
		    (DHCP_BASE_ATTR(vp->attribute) != PW_DHCP_OPTION_82)) goto next;

		length = vp->length;

		for (same = vp->next; same != NULL; same = same->next) {
			if (same->attribute != vp->attribute) break;
			num_entries++;
		}

		/*
		 *	For client-identifier
		 */
		if ((vp->type == PW_TYPE_ETHERNET) &&
		    (vp->length == 6) &&
		    (num_entries == 1)) {
			vp->type = PW_TYPE_OCTETS;
			memmove(vp->vp_octets + 1, vp->vp_octets, 6);
			vp->vp_octets[0] = 1;
		}

		pattr = p;
		*(p++) = vp->attribute & 0xff;
		plength = p;
		*(p++) = 0;	/* header isn't included in attr length */

		if (DHCP_BASE_ATTR(vp->attribute) == PW_DHCP_OPTION_82) {
			*(p++) = DHCP_UNPACK_OPTION1(vp->attribute);
			*(p++) = 0;
			*plength = 2;
		}

		for (i = 0; i < num_entries; i++) {
			if (librad_debug > 1) {
				vp_prints(buffer, sizeof(buffer), vp);
				fprintf(stderr, "\t%s\n", buffer);
			}

			length = fr_dhcp_vp2attr(vp, p, 0);

			/*
			 *	This will never happen due to FreeRADIUS
			 *	limitations: sizeof(vp->vp_octets) < 255
			 */
			if (length > 255) {
				fprintf(stderr, "WARNING Ignoring too long attribute %s!\n", vp->name);
				break;
			}

			/*
			 *	More than one attribute of the same type
			 *	in a row: they are packed together
			 *	into the same TLV.  If we overflow,
			 *	go bananas!
			 */
			if ((*plength + length) > 255) {
				fprintf(stderr, "WARNING Ignoring too long attribute %s!\n", vp->name);
				break;
			}
			
			*plength += length;
			p += length;

			if (vp->next &&
			    (vp->next->attribute == vp->attribute))
				vp = vp->next;
		} /* loop over num_entries */

		if (DHCP_BASE_ATTR(vp->attribute) == PW_DHCP_OPTION_82) {
			plength[2] = plength[0] - 2;
		}

	next:
		vp = vp->next;
	}

	p[0] = 0xff;		/* end of option option */
	p[1] = 0x00;
	p += 2;
	dhcp_size = p - packet->data;

	/*
	 *	FIXME: if (dhcp_size > mms),
	 *	  then we put the extra options into the "sname" and "file"
	 *	  fields, AND set the "end option option" in the "options"
	 *	  field.  We also set the "overload option",
	 *	  and put options into the "file" field, followed by
	 *	  the "sname" field.  Where each option is completely
	 *	  enclosed in the "file" and/or "sname" field, AND
	 *	  followed by the "end of option", and MUST be followed
	 *	  by padding option.
	 *
	 *	Yuck.  That sucks...
	 */
	packet->data_len = dhcp_size;

	packet->dst_ipaddr.af = AF_INET;
	packet->src_ipaddr.af = AF_INET;

	packet->dst_port = original->src_port;
	packet->src_port = original->dst_port;

	/*
	 *	Note that for DHCP, we NEVER send the response to the
	 *	source IP address of the request.  It may have
	 *	traversed multiple relays, and we need to send the request
	 *	to the relay closest to the client.
	 *
	 *	if giaddr, send to giaddr.
	 *	if NAK, send broadcast packet
	 *	if ciaddr, unicast to ciaddr
	 *	if flags & 0x8000, broadcast (client request)
	 *	if sent from 0.0.0.0, broadcast response
	 *	unicast to client yiaddr
	 */

	/*
	 *	FIXME: alignment issues.  We likely don't want to
	 *	de-reference the packet structure directly..
	 */
	dhcp = (dhcp_packet_t *) original->data;

	if (dhcp->giaddr != htonl(INADDR_ANY)) {
		packet->dst_ipaddr.ipaddr.ip4addr.s_addr = dhcp->giaddr;

	} else if (packet->code == PW_DHCP_NAK) {
		packet->dst_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_BROADCAST);
		
	} else if (dhcp->ciaddr != htonl(INADDR_ANY)) {
		packet->dst_ipaddr.ipaddr.ip4addr.s_addr = dhcp->ciaddr;

	} else if ((dhcp->flags & 0x8000) != 0) {
		packet->dst_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_BROADCAST);

	} else if (packet->dst_ipaddr.ipaddr.ip4addr.s_addr == htonl(INADDR_ANY)) {
		packet->dst_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_BROADCAST);

	} else {
		packet->dst_ipaddr.ipaddr.ip4addr.s_addr = dhcp->yiaddr;
	}

	/*
	 *	FIXME: This may set it to broadcast, which we don't
	 *	want.  Instead, set it to the real address of the
	 *	socket.
	 */
	packet->src_ipaddr = original->dst_ipaddr;

	packet->sockfd = original->sockfd;

	if (packet->data_len < DEFAULT_PACKET_SIZE) {
		memset(packet->data + packet->data_len, 0,
		       DEFAULT_PACKET_SIZE - packet->data_len);
		packet->data_len = DEFAULT_PACKET_SIZE;
	}

	if ((librad_debug > 2) && fr_log_fp) {
		for (i = 0; i < packet->data_len; i++) {
			if ((i & 0x0f) == 0x00) fprintf(fr_log_fp, "%d: ", i);
			fprintf(fr_log_fp, "%02x ", packet->data[i]);
			if ((i & 0x0f) == 0x0f) fprintf(fr_log_fp, "\n");
		}
		fprintf(fr_log_fp, "\n");
	}

	return 0;
}
#endif /* WITH_DHCP */
