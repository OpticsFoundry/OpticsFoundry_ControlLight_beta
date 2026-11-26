/*
 * Sequencer identity (MAC address from the QSPI flash unique ID) and UDP discovery (2026-11-26),
 * see sequencer_identity.h
 */

#include <stdio.h>
#include <string.h>
#include "xil_printf.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "sequencer_identity.h"
#include "firmware_update.h"

extern struct netif *echo_netif;   /* main.c */

/* 1: MAC address from the QSPI flash unique ID; 0: always the old fixed MAC address */
#define USE_FLASH_UNIQUE_ID_FOR_MAC 1

static unsigned char Mac[6] = { 0x00, 0x0A, 0x35, 0xF1, 0x01, 0x03 };  /* fallback: the old fixed MAC address */
static bool MacFromUniqueId = false;
static bool MacComputed = false;
static struct udp_pcb* DiscoveryPcb = NULL;

void SequencerIdentity_get_mac(unsigned char mac[6]) {
	if (!MacComputed) {
		MacComputed = true;
		u8 unique_id[8];
		if (USE_FLASH_UNIQUE_ID_FOR_MAC && FirmwareUpdate_read_flash_unique_id(unique_id)) {
			u32 hash = FirmwareUpdate_crc32(unique_id, sizeof(unique_id));
			Mac[0] = 0x02;  /* locally administered, unicast */
			Mac[1] = 0x4F;  /* 'O' */
			Mac[2] = 0x46;  /* 'F' */
			Mac[3] = (u8)(hash >> 16);
			Mac[4] = (u8)(hash >> 8);
			Mac[5] = (u8)(hash);
			MacFromUniqueId = true;
		} else {
			xil_printf("SequencerIdentity: using fixed MAC address\r\n");
		}
	}
	memcpy(mac, Mac, sizeof(Mac));
}

bool SequencerIdentity_mac_from_unique_id(void) {
	return MacFromUniqueId;
}

void SequencerIdentity_describe(char* text, size_t size) {
	const ip4_addr_t* ip = (echo_netif != NULL) ? netif_ip4_addr(echo_netif) : NULL;
	if (ip != NULL) {
		snprintf(text, size, "MAC %02X:%02X:%02X:%02X:%02X:%02X%s; IP %d.%d.%d.%d",
				Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5], MacFromUniqueId ? "" : " (fixed)",
				ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
	} else {
		snprintf(text, size, "MAC %02X:%02X:%02X:%02X:%02X:%02X; IP -", Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]);
	}
}

static void DiscoveryReceive(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
	if (p == NULL) return;
	char request[64];
	u16_t length = pbuf_copy_partial(p, request, sizeof(request) - 1, 0);
	pbuf_free(p);
	request[length] = 0;
	while ((length > 0) && ((request[length - 1] == '\n') || (request[length - 1] == '\r'))) request[--length] = 0;
	if (strcmp(request, SEQUENCER_DISCOVERY_REQUEST) != 0) return;

	char identity[160];
	char firmware[256];
	char reply[460];
	SequencerIdentity_describe(identity, sizeof(identity));
	FirmwareUpdate_describe(firmware, sizeof(firmware));
	int reply_length = snprintf(reply, sizeof(reply), "OpticsFoundry sequencer; %s; TCP port %d; %s", identity, SEQUENCER_TCP_PORT, firmware);
	if (reply_length <= 0) return;
	if (reply_length >= (int)sizeof(reply)) reply_length = sizeof(reply) - 1;

	struct pbuf* answer = pbuf_alloc(PBUF_TRANSPORT, (u16_t)reply_length, PBUF_RAM);
	if (answer == NULL) return;
	memcpy(answer->payload, reply, reply_length);
	/* unicast if the requester is in our subnet; otherwise (e.g. our IP belongs to another subnet) broadcast */
	const ip4_addr_t* own_ip = netif_ip4_addr(echo_netif);
	const ip4_addr_t* own_mask = netif_ip4_netmask(echo_netif);
	if (!ip4_addr_isany(own_ip) && ip4_addr_netcmp(ip_2_ip4(addr), own_ip, own_mask)) {
		udp_sendto(pcb, answer, addr, port);
	} else {
		udp_sendto(pcb, answer, IP_ADDR_BROADCAST, port);
	}
	pbuf_free(answer);
}

void SequencerIdentity_start_discovery(void) {
	if (DiscoveryPcb != NULL) return;
	DiscoveryPcb = udp_new();
	if (DiscoveryPcb == NULL) {
		xil_printf("SequencerIdentity: can't create discovery UDP PCB\r\n");
		return;
	}
	if (udp_bind(DiscoveryPcb, IP_ADDR_ANY, SEQUENCER_DISCOVERY_UDP_PORT) != ERR_OK) {
		xil_printf("SequencerIdentity: can't bind discovery UDP port %d\r\n", SEQUENCER_DISCOVERY_UDP_PORT);
		udp_remove(DiscoveryPcb);
		DiscoveryPcb = NULL;
		return;
	}
	udp_recv(DiscoveryPcb, DiscoveryReceive, NULL);
	xil_printf("discovery: listening on UDP port %d\r\n", SEQUENCER_DISCOVERY_UDP_PORT);
}
