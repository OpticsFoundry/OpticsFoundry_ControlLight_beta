#pragma once
/*
 * Network settings of the sequencer: DHCP or fixed IP address (2026-11-27)
 *
 * Address at start-up
 *   DHCP (default): the sequencer asks for an address by DHCP. If no DHCP server answers within 3 s, it uses a
 *     link-local address 169.254.x.y (netmask 255.255.0.0, x and y derived from the MAC address, so always the same
 *     for one board), like a PC does without DHCP server. This way a PC connected directly to the sequencer by
 *     cable, without DHCP server, finds it without configuration. DHCP keeps running in the background: when a
 *     DHCP server appears later, the sequencer switches to the address it offers. If a DHCP lease is lost, the
 *     sequencer returns to its link-local address after 3 s.
 *   Fixed IP: the stored IP address, netmask and gateway (gateway 0.0.0.0: none). No DHCP.
 *   Changed settings take effect after the next reboot (reboot_system).
 *
 * Storage: settings record (128 bytes, CRC32 protected, with a sequence counter) in the QSPI settings sector,
 *   alternately in the 4 KB subsectors at 0xFF0000 and 0xFF1000, so that the previous settings survive a power
 *   failure while writing. No valid record: DHCP. The record has reserved space for later settings, e.g. a pairing key.
 *
 * Recovery, if the sequencer can't be reached any more with its fixed IP address:
 *   - discovery still finds it (the answer is broadcast if the PC is in another subnet), and the UDP command
 *     OpticsFoundry_set_network below sets DHCP or another fixed IP address, addressed by MAC address;
 *   - or hold the Z-turn USR button (S1, MIO50) while switching the sequencer on, until it beeps twice (about 2 s
 *     after power-on; RGB LED yellow while held, then green): this sets the network settings back to DHCP.
 *
 * Settings text (used by the commands below):
 *   "dhcp"
 *   "fixed <IP address> <netmask> [<gateway>]"      e.g. "fixed 192.168.1.10 255.255.255.0" or "fixed 10.0.0.5 255.255.0.0 10.0.0.1"
 *   Refused: invalid netmask, IP addresses 0.x.x.x, 127.x.x.x, 169.254.x.x (link-local, chosen automatically),
 *   224.x.x.x and above, the network or broadcast address of the subnet, a gateway outside the subnet.
 *
 * TCP commands (dispatched in FireflyControlLoop):
 *   get_network_settings
 *       -> "OK stored setting <DHCP | fixed IP a.b.c.d, netmask ..., gateway ...>; current IP ...; netmask ...; gateway ...; IP source ..."
 *   set_network_settings
 *       <settings text>\n
 *       -> "OK stored ...; takes effect after the next reboot" or "ERROR ..."
 *
 * UDP command, to the discovery port 57979 (usually as broadcast, so that it also reaches sequencers in another subnet):
 *   "OpticsFoundry_set_network <MAC address> <settings text>"     e.g. "OpticsFoundry_set_network 02:4F:46:4C:B5:52 dhcp"
 *   Only the sequencer with that MAC address reacts. It stores the settings and reboots, after answering
 *   (by unicast if the PC is in its subnet, otherwise by broadcast, to the sender's port):
 *       "OpticsFoundry set_network; MAC ...; OK stored ...; rebooting"   or   "OpticsFoundry set_network; MAC ...; ERROR ..."
 *   Refused while a sequence is running.
 */

#include <stdbool.h>
#include <stddef.h>
#include "lwip/ip_addr.h"

/* Call at start-up, after FireflyControl_init_hardware and FirmwareUpdate_init and before the network is set up:
 * reads the stored settings and checks whether the USR button is held. */
void NetworkSettings_init(void);

/* true if a fixed IP address is configured; then fills ip, netmask and gateway */
bool NetworkSettings_use_fixed_ip(ip4_addr_t* ip, ip4_addr_t* netmask, ip4_addr_t* gateway);

/* the link-local address of this board (169.254.x.y / 255.255.0.0, no gateway) */
void NetworkSettings_link_local_address(ip4_addr_t* ip, ip4_addr_t* netmask, ip4_addr_t* gateway);

/* "netmask ...; gateway ...; IP source <DHCP | fixed | link-local (no DHCP server) | none>" */
void NetworkSettings_describe_current(char* text, size_t size);

/* TCP commands */
void NetworkSettings_get_command(void);
void NetworkSettings_set_command(bool sequence_running);

/* Called by the discovery UDP receiver for every datagram. Returns true if it was an OpticsFoundry_set_network request. */
bool NetworkSettings_handle_udp_request(const char* request, const ip_addr_t* addr, u16_t port);

/* Called by RunEventLoop (not from within the lwIP receive callback): reboots after the UDP command,
 * and returns to the link-local address when a DHCP lease was lost. */
void NetworkSettings_poll(void);

/* state of the Z-turn USR button (S1, MIO50, low when pressed) */
bool UserButton_pressed(void);
