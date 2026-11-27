/*
 * Copyright (C) 2009 - 2019 Xilinx, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */

#include <stdio.h>

#include "xparameters.h"

#include "netif/xadapter.h"

#include "platform.h"
#include "platform_config.h"
#if defined (__arm__) || defined(__aarch64__)
#include "xil_printf.h"
#endif

#include "lwip/tcp.h"
#include "xil_cache.h"

#if LWIP_IPV6==1
#include "lwip/ip.h"
#else
#if LWIP_DHCP==1
#include "lwip/dhcp.h"
#endif
#endif

#include "FireflyControl.h"
#include "firmware_update.h"
#include "sequencer_identity.h"
#include "sd_installer.h"
#include "network_settings.h"
#include "xtime_l.h"

/* defined by each RAW mode application */
void print_app_header();
int start_application();
int transfer_data();
void tcp_fasttmr(void);
void tcp_slowtmr(void);

/* missing declaration in lwIP */
void lwip_init();

#if LWIP_IPV6==0
#if LWIP_DHCP==1
extern volatile int dhcp_timoutcntr;
err_t dhcp_start(struct netif *netif);
#endif
#endif

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
static struct netif server_netif;
struct netif *echo_netif;

#if LWIP_IPV6==1
void print_ip6(char *msg, ip_addr_t *ip)
{
	print(msg);
	xil_printf(" %x:%x:%x:%x:%x:%x:%x:%x\n\r",
			IP6_ADDR_BLOCK1(&ip->u_addr.ip6),
			IP6_ADDR_BLOCK2(&ip->u_addr.ip6),
			IP6_ADDR_BLOCK3(&ip->u_addr.ip6),
			IP6_ADDR_BLOCK4(&ip->u_addr.ip6),
			IP6_ADDR_BLOCK5(&ip->u_addr.ip6),
			IP6_ADDR_BLOCK6(&ip->u_addr.ip6),
			IP6_ADDR_BLOCK7(&ip->u_addr.ip6),
			IP6_ADDR_BLOCK8(&ip->u_addr.ip6));

}
#else
void
print_ip(char *msg, ip_addr_t *ip)
{
	print(msg);
	xil_printf("%d.%d.%d.%d\n\r", ip4_addr1(ip), ip4_addr2(ip),
			ip4_addr3(ip), ip4_addr4(ip));
}

void
print_ip_settings(ip_addr_t *ip, ip_addr_t *mask, ip_addr_t *gw)
{

	print_ip("Board IP: ", ip);
	print_ip("Netmask : ", mask);
	print_ip("Gateway : ", gw);
}
#endif

#if defined (__arm__) && !defined (ARMR5)
#if XPAR_GIGE_PCS_PMA_SGMII_CORE_PRESENT == 1 || XPAR_GIGE_PCS_PMA_1000BASEX_CORE_PRESENT == 1
int ProgramSi5324(void);
int ProgramSfpPhy(void);
#endif
#endif

#ifdef XPS_BOARD_ZCU102
#ifdef XPAR_XIICPS_0_DEVICE_ID
int IicPhyReset(void);
#endif
#endif


void ShowSignOfLife() {
	static u32 counter = 0;
	switch (counter) {
		case 0: xil_printf("\b-");break;
		case 1: xil_printf("\b\\");break;
		case 2: xil_printf("\b");xil_printf("|");break;// | doesn't print in Termite if in same xil_printf command
		case 3: xil_printf("\b/");break;
	}
	counter++;
	if (counter >= 4) {
		counter = 0;
	}
}

void RunEventLoop() { //make sure other parts of the program regularly (at least every 10ms) call this function while they are waiting for data to be received or sent.
	if (TcpFastTmrFlag) {
		tcp_fasttmr();
		FireflyFastLoop(echo_netif);
		TcpFastTmrFlag = 0;
	}
	if (TcpSlowTmrFlag) {
		tcp_slowtmr();
		//if (DebugModeOn) ShowSignOfLife();
		TcpSlowTmrFlag = 0;
	}
	xemacif_input(echo_netif);
	NetworkSettings_poll();   /* reboot requested over UDP, link-local address after a lost DHCP lease */
	transfer_data();
	FireflyControlLoop();
}


/* the mac address of the board. this should be unique per board */
/*
Since 2026-11-26 the MAC address is computed at start-up from the unique ID of the QSPI flash,
see sequencer_identity.h: 02:4F:46:xx:xx:xx. The value below is only used if the flash has no unique ID.
*/
unsigned char mac_ethernet_address[] =
	{ 0x00, 0x0a, 0x35, 0xF1, 0x01, 0x03 };

void get_mac_address_as_string(char * buf) {
	sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X\n",mac_ethernet_address[0],mac_ethernet_address[1],mac_ethernet_address[2],mac_ethernet_address[3],mac_ethernet_address[4],mac_ethernet_address[5]);
}


int main()
{


#if LWIP_IPV6==0
	ip_addr_t ipaddr, netmask, gw;

#endif
	
	echo_netif = &server_netif;
#if defined (__arm__) && !defined (ARMR5)
#if XPAR_GIGE_PCS_PMA_SGMII_CORE_PRESENT == 1 || XPAR_GIGE_PCS_PMA_1000BASEX_CORE_PRESENT == 1
	ProgramSi5324();
	ProgramSfpPhy();
#endif
#endif

/* Define this board specific macro in order perform PHY reset on ZCU102 */
#ifdef XPS_BOARD_ZCU102
	if(IicPhyReset()) {
		xil_printf("Error performing PHY reset \n\r");
		return -1;
	}
#endif

	init_platform();

	//cleanup_platform();//after one forum entry supposedly needed to make timer work; not sure if true


/*
To change MAC address: 

Change mac_ethernet_address above, then rebuild as DEBUG and RELEASE, create firmware image, flash.


To switch between static IP and DCHP:

For static IP: set IP parameters in the table below.

Method 1 (GUI):

Open BSP settings for lwIP, i.e.
In "Explorer" window, open open "OpticsFoundry_Seq...Platform"
Double click on platform.spr
Select "standalone... -> Board Support Package"
Click "Modify BSP Settings"
Click "Standalone -> lwip213"
Under "DHCP options" put DHCP_DOES_ARP_CHECK and LWIP_DHCP on "false" for Static_IP and on "true" for DHCP.

Method 2 (text editor):

In lwipopts.h (appears twice in project, search for �#define LWIP_DHCP� in whole project
with Visual Studio Code)

for DHCP:
#define LWIP_DHCP 1
#define DHCP_DOES_ARP_CHECK 1

for StaticIP:
#define LWIP_DHCP 0
#define DHCP_DOES_ARP_CHECK 0

Clean project.
Set DEBUG active
Rebuild as DEBUG (even if you use RELEASE)
Check that under "Problem" you see the information if STATIC_IP or DHCP is used.
Set RELEASE active
Rebuild as RELEASE
Check that under "Problem" you see the information if STATIC_IP or DHCP is used.
Create firmware image
Flash


*/

/*
 If you get the problem that the math library is not found, e.g. "cmd_acos" not available:
 Right click "OpticsFoundry_Seq...App" (not "..App_system")
 Select "Properties"
 Select "C/C++ Build -> Settings"
 Select "ARM v7 gcc linker -> Inferred Options -> Software Platform"
 Put the following under "Software Platform Inferred Flags":
-Wl,--start-group,-lxil,-lgcc,-lc,-lm,--end-group
-Wl,--start-group,-lxil,-llwip4,-lgcc,-lc,-lm,--end-group

 */

/*
 If your ethernet speed is slow, make sure to set the BSP settings for lwip correctly,
	mem_size 			524288
	memp_n_pbuf 		1024
	memp_n_tcp_seg 		1024
	pbuf_pool_size 		16384
	tcp_snd_buf 		65535
	tcp_wnd 			65535
	n_rx_descriptors 	512
	n_tx_descriptors 	512

The complete list is shown in "MicroZed ethernet lwIP BSP working vs not working.pptx".
 */

#if LWIP_IPV6==0
#if LWIP_DHCP==1
    ipaddr.addr = 0;
	gw.addr = 0;
	netmask.addr = 0;
#warning "Information: Using DHCP, not static IP."
#else
	/* initialize IP addresses to be used */
	IP4_ADDR(&ipaddr,  192, 168,   1, 10);
	IP4_ADDR(&netmask, 255, 255, 255,  0);
	/* No default gateway for isolation */
	IP4_ADDR(&gw,      0,   0,   0,  0);
	//Otherwise, if you want the Zynq to access the internet beyond your local ethernet, specify your gateway, e.g.
	//IP4_ADDR(&gw,      192, 168,   1,  1);
#warning "Information: Using static IP, not DHCP."
#endif
#endif
	print_app_header();

	/* AXI GPIOs to the FPGA core (status LED, buzzer) */
	FireflyControl_init_hardware();

	/* board specific MAC address, derived from the QSPI flash unique ID */
	FirmwareUpdate_init();
	/* when booted from an installer SD card (BOOT.BIN + INSTALL file): copy BOOT.BIN into the QSPI flash */
	SdInstaller_run_at_boot();
	SequencerIdentity_get_mac(mac_ethernet_address);
	xil_printf("MAC address %02X:%02X:%02X:%02X:%02X:%02X\r\n", mac_ethernet_address[0], mac_ethernet_address[1],
			mac_ethernet_address[2], mac_ethernet_address[3], mac_ethernet_address[4], mac_ethernet_address[5]);
	/* DHCP or fixed IP address, stored in the QSPI flash; USR button held at start-up: back to DHCP (2026-11-27) */
	NetworkSettings_init();
	bool fixed_ip = false;
#if (LWIP_IPV6 == 0)
	fixed_ip = NetworkSettings_use_fixed_ip(&ipaddr, &netmask, &gw);
#endif

	lwip_init();

#if (LWIP_IPV6 == 0)
	/* Add network interface to the netif_list, and set it as default */
	if (!xemac_add(echo_netif, &ipaddr, &netmask,
						&gw, mac_ethernet_address,
						PLATFORM_EMAC_BASEADDR)) {
		xil_printf("Error adding N/W interface\n\r");
		return -1;
	}
#else
	/* Add network interface to the netif_list, and set it as default */
	if (!xemac_add(echo_netif, NULL, NULL, NULL, mac_ethernet_address,
						PLATFORM_EMAC_BASEADDR)) {
		xil_printf("Error adding N/W interface\n\r");
		return -1;
	}
	echo_netif->ip6_autoconfig_enabled = 1;

	netif_create_ip6_linklocal_address(echo_netif, 1);
	netif_ip6_addr_set_state(echo_netif, 0, IP6_ADDR_VALID);

	print_ip6("\n\rBoard IPv6 address ", &echo_netif->ip6_addr[0].u_addr.ip6);

#endif
	netif_set_default(echo_netif);

	/* now enable interrupts */
	platform_enable_interrupts();

	/* specify that the network if is up */
	netif_set_up(echo_netif);

#if (LWIP_IPV6 == 0)
#if (LWIP_DHCP==1)
	/* Create a new DHCP client for this interface.
	 * Note: you must call dhcp_fine_tmr() and dhcp_coarse_tmr() at
	 * the predefined regular intervals after starting the client.
	 */
	if (fixed_ip) {
		xil_printf("using the fixed IP address from the network settings, no DHCP\r\n");
	} else {
		/* 2026-11-27: wait up to DHCP_WAIT_MS (real time; the lwIP timers run faster than nominal, see platform_zynq.c).
		 * Without answer, use the link-local address 169.254.x.y, like a PC without DHCP server (previously: 192.168.1.10).
		 * DHCP continues in the background and replaces the link-local address when a server answers. */
		#define DHCP_WAIT_MS 3000
		dhcp_start(echo_netif);
		XTime dhcp_start_time, now;
		XTime_GetTime(&dhcp_start_time);
		do {
			xemacif_input(echo_netif);
			XTime_GetTime(&now);
		} while ((echo_netif->ip_addr.addr == 0) && ((now - dhcp_start_time) < (XTime)DHCP_WAIT_MS * (COUNTS_PER_SECOND / 1000)));

		if (echo_netif->ip_addr.addr == 0) {
			ip4_addr_t ll_ip, ll_netmask, ll_gw;
			NetworkSettings_link_local_address(&ll_ip, &ll_netmask, &ll_gw);
			netif_set_addr(echo_netif, &ll_ip, &ll_netmask, &ll_gw);
			xil_printf("DHCP timeout after %d ms: using the link-local address\r\n", DHCP_WAIT_MS);
		} else {
			xil_printf("DHCP address received after %lu ms\r\n", (unsigned long)((now - dhcp_start_time) / (COUNTS_PER_SECOND / 1000)));
		}
	}

	ipaddr.addr = echo_netif->ip_addr.addr;
	gw.addr = echo_netif->gw.addr;
	netmask.addr = echo_netif->netmask.addr;
#endif

	print_ip_settings(&ipaddr, &netmask, &gw);

#endif
	/* start the application (web server, rxtest, txtest, etc..) */
	start_application();

	/* receive and process packets */
	while (1) {
		RunEventLoop();
	}

	/* never reached */
	cleanup_platform();

	return 0;
}
