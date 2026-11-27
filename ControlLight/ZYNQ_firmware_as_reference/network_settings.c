/*
 * Network settings: DHCP or fixed IP address, link-local fallback, USR button reset (2026-11-27), see network_settings.h
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "xparameters.h"
#include "xgpiops.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "echo.h"
#include "FireflyControl.h"
#include "firmware_update.h"
#include "sequencer_identity.h"
#include "network_settings.h"

extern struct netif *echo_netif;    /* main.c */
extern bool Sequence_running;       /* FireflyControl.c */
extern struct tcp_pcb *last_tpcb;   /* echo.c */

#define SETTINGS_MAGIC          0x4553464Fu  /* "OFSE" */
#define SETTINGS_VERSION        1
#define SETTINGS_COPY_SIZE      0x1000       /* one copy per 4 KB subsector */
#define SETTINGS_COPIES         2

#define NETWORK_MODE_DHCP       0
#define NETWORK_MODE_FIXED      1

#define USER_BUTTON_MIO_PIN     50
#define USER_BUTTON_HOLD_MS     1000         /* the button must be held this long at start-up to reset the settings */
#define LINK_LOCAL_DELAY_MS     3000         /* without DHCP address for this long: use the link-local address */

/* Stored in the QSPI flash. New fields go into reserved[] (which reads 0 in older records), so the layout stays compatible. */
typedef struct {
	u32 magic;
	u32 version;
	u32 sequence;        /* incremented with every write; the valid copy with the highest value is used */
	u32 network_mode;    /* NETWORK_MODE_ */
	u32 ip;              /* lwIP ip4_addr_t.addr (network byte order) */
	u32 netmask;
	u32 gateway;
	u32 reserved[24];
	u32 crc;             /* CRC32 of all preceding bytes */
} SettingsRecord;

typedef char SettingsRecordSizeCheck[(sizeof(SettingsRecord) == 128) ? 1 : -1];

static SettingsRecord Stored;        /* the current content of the flash (or defaults) */
static int StoredCopy = -1;          /* subsector of Stored, -1: none valid */
static SettingsRecord Active;        /* the settings used since start-up */
static bool ButtonResetAtBoot = false;

static XGpioPs ButtonGpio;
static bool ButtonGpioReady = false;

/* reboot requested by the UDP command, executed by NetworkSettings_poll */
static bool RebootPending = false;
static bool Rebooting = false;

static XTime NoAddressSince = 0;

/* ---------------- helpers ---------------- */

static u32 ElapsedMs(XTime start) {
	XTime now;
	XTime_GetTime(&now);
	return (u32)((now - start) / (COUNTS_PER_SECOND / 1000));
}

static void WaitMs(u32 ms) {
	XTime start;
	XTime_GetTime(&start);
	while (ElapsedMs(start) < ms);
}

static void Beep(int count, u32 on_ms, u32 off_ms) {
	for (int i = 0; i < count; i++) {
		SwitchBuzzer(1);
		WaitMs(on_ms);
		SwitchBuzzer(0);
		WaitMs(off_ms);
	}
}

static void FormatIp(char* text, size_t size, u32 address) {
	ip4_addr_t ip;
	ip.addr = address;
	snprintf(text, size, "%d.%d.%d.%d", ip4_addr1(&ip), ip4_addr2(&ip), ip4_addr3(&ip), ip4_addr4(&ip));
}

/* "DHCP" or "fixed IP a.b.c.d, netmask ..., gateway ..." */
static void DescribeRecord(char* text, size_t size, const SettingsRecord* record) {
	if (record->network_mode != NETWORK_MODE_FIXED) {
		snprintf(text, size, "DHCP");
		return;
	}
	char ip[16], netmask[16], gateway[16];
	FormatIp(ip, sizeof(ip), record->ip);
	FormatIp(netmask, sizeof(netmask), record->netmask);
	if (record->gateway == 0) snprintf(gateway, sizeof(gateway), "none");
	else FormatIp(gateway, sizeof(gateway), record->gateway);
	snprintf(text, size, "fixed IP %s, netmask %s, gateway %s", ip, netmask, gateway);
}

static bool SameNetworkSettings(const SettingsRecord* a, const SettingsRecord* b) {
	if (a->network_mode != b->network_mode) return false;
	if (a->network_mode != NETWORK_MODE_FIXED) return true;
	return (a->ip == b->ip) && (a->netmask == b->netmask) && (a->gateway == b->gateway);
}

static void SetDefaults(SettingsRecord* record) {
	memset(record, 0, sizeof(*record));
	record->magic = SETTINGS_MAGIC;
	record->version = SETTINGS_VERSION;
	record->network_mode = NETWORK_MODE_DHCP;
}

static bool RecordValid(const SettingsRecord* record) {
	if ((record->magic != SETTINGS_MAGIC) || (record->version < 1)) return false;
	return FirmwareUpdate_crc32((const u8*)record, offsetof(SettingsRecord, crc)) == record->crc;
}

static void ReadStoredSettings(void) {
	SetDefaults(&Stored);
	StoredCopy = -1;
	for (int copy = 0; copy < SETTINGS_COPIES; copy++) {
		SettingsRecord record;
		if (!FirmwareUpdate_read_settings(copy * SETTINGS_COPY_SIZE, (u8*)&record, sizeof(record))) continue;
		if (!RecordValid(&record)) continue;
		if ((StoredCopy < 0) || ((s32)(record.sequence - Stored.sequence) > 0)) {
			Stored = record;
			StoredCopy = copy;
		}
	}
}

/* writes record into the copy not holding the current settings; fills error on failure */
static bool WriteSettings(SettingsRecord* record, char* error, size_t error_size) {
	record->magic = SETTINGS_MAGIC;
	if (record->version < SETTINGS_VERSION) record->version = SETTINGS_VERSION;
	record->sequence = Stored.sequence + 1;
	record->crc = FirmwareUpdate_crc32((const u8*)record, offsetof(SettingsRecord, crc));
	int copy = (StoredCopy == 0) ? 1 : 0;
	if (!FirmwareUpdate_write_settings(copy * SETTINGS_COPY_SIZE, (const u8*)record, sizeof(*record))) {
		snprintf(error, error_size, "writing the settings into the QSPI flash failed (flash busy, write protected or defect)");
		return false;
	}
	ReadStoredSettings();
	if ((StoredCopy != copy) || (memcmp(&Stored, record, sizeof(*record)) != 0)) {
		snprintf(error, error_size, "the settings read back from the QSPI flash differ from the written ones");
		return false;
	}
	return true;
}

/* ---------------- parsing and checking settings texts ---------------- */

static const char* SkipSpaces(const char* p) {
	while ((*p == ' ') || (*p == '\t')) p++;
	return p;
}

/* strict dotted quad "a.b.c.d"; advances *p */
static bool ParseIp(const char** p, u32* address) {
	const char* s = SkipSpaces(*p);
	u32 bytes[4];
	for (int i = 0; i < 4; i++) {
		if (!isdigit((unsigned char)*s)) return false;
		u32 value = 0;
		int digits = 0;
		while (isdigit((unsigned char)*s)) {
			value = value * 10 + (u32)(*s - '0');
			s++;
			if ((++digits > 3) || (value > 255)) return false;
		}
		bytes[i] = value;
		if (i < 3) {
			if (*s != '.') return false;
			s++;
		}
	}
	if ((*s != 0) && (*s != ' ') && (*s != '\t') && (*s != '\r') && (*s != '\n')) return false;
	ip4_addr_t ip;
	IP4_ADDR(&ip, bytes[0], bytes[1], bytes[2], bytes[3]);
	*address = ip.addr;
	*p = s;
	return true;
}

static bool AtEnd(const char* p) {
	p = SkipSpaces(p);
	return (*p == 0) || (*p == '\r') || (*p == '\n');
}

static bool StartsWithWord(const char** p, const char* word) {
	const char* s = SkipSpaces(*p);
	size_t length = strlen(word);
	for (size_t i = 0; i < length; i++) if (tolower((unsigned char)s[i]) != word[i]) return false;
	if ((s[length] != 0) && (s[length] != ' ') && (s[length] != '\t') && (s[length] != '\r') && (s[length] != '\n')) return false;
	*p = s + length;
	return true;
}

/* parses and checks a settings text into record (only the network fields are changed) */
static bool ParseSettings(const char* text, SettingsRecord* record, char* error, size_t error_size) {
	const char* p = text;
	if (StartsWithWord(&p, "dhcp")) {
		if (!AtEnd(p)) {
			snprintf(error, error_size, "unexpected text after \"dhcp\"");
			return false;
		}
		record->network_mode = NETWORK_MODE_DHCP;
		record->ip = record->netmask = record->gateway = 0;
		return true;
	}
	if (!StartsWithWord(&p, "fixed")) {
		snprintf(error, error_size, "expected \"dhcp\" or \"fixed <IP address> <netmask> [<gateway>]\"");
		return false;
	}
	u32 ip, netmask, gateway = 0;
	if (!ParseIp(&p, &ip)) {
		snprintf(error, error_size, "invalid IP address (expected a.b.c.d)");
		return false;
	}
	if (!ParseIp(&p, &netmask)) {
		snprintf(error, error_size, "invalid or missing netmask (expected e.g. 255.255.255.0)");
		return false;
	}
	if (!AtEnd(p) && !ParseIp(&p, &gateway)) {
		snprintf(error, error_size, "invalid gateway (expected a.b.c.d, or leave it out for none)");
		return false;
	}
	if (!AtEnd(p)) {
		snprintf(error, error_size, "unexpected text after the gateway");
		return false;
	}
	/* checks, in host byte order */
	u32 h_ip = lwip_ntohl(ip), h_mask = lwip_ntohl(netmask), h_gw = lwip_ntohl(gateway);
	u32 inverted = ~h_mask;
	if ((h_mask == 0) || ((inverted & (inverted + 1)) != 0) || (inverted < 3)) {
		snprintf(error, error_size, "invalid netmask (must be contiguous, from 128.0.0.0 to 255.255.255.252)");
		return false;
	}
	u32 first = h_ip >> 24;
	if ((first == 0) || (first == 127) || (first >= 224)) {
		snprintf(error, error_size, "IP address not allowed (0.x.x.x, 127.x.x.x and 224.x.x.x and above are reserved)");
		return false;
	}
	if ((h_ip >> 16) == 0xA9FE) {
		snprintf(error, error_size, "IP address not allowed (169.254.x.x is chosen automatically when DHCP fails)");
		return false;
	}
	if (((h_ip & inverted) == 0) || ((h_ip & inverted) == inverted)) {
		snprintf(error, error_size, "IP address is the network or broadcast address of its subnet");
		return false;
	}
	if (h_gw != 0) {
		if ((h_gw & h_mask) != (h_ip & h_mask)) {
			snprintf(error, error_size, "gateway is not in the subnet of the IP address");
			return false;
		}
		if (((h_gw & inverted) == 0) || ((h_gw & inverted) == inverted) || (h_gw == h_ip)) {
			snprintf(error, error_size, "invalid gateway (network or broadcast address, or the sequencer's own address)");
			return false;
		}
	}
	record->network_mode = NETWORK_MODE_FIXED;
	record->ip = ip;
	record->netmask = netmask;
	record->gateway = gateway;
	return true;
}

/* ---------------- USR button ---------------- */

static void UserButtonInit(void) {
	if (ButtonGpioReady) return;
	XGpioPs_Config* config = XGpioPs_LookupConfig(XPAR_XGPIOPS_0_DEVICE_ID);
	if ((config == NULL) || (XGpioPs_CfgInitialize(&ButtonGpio, config, config->BaseAddr) != XST_SUCCESS)) {
		xil_printf("NetworkSettings: PS GPIO initialization failed, USR button not available\r\n");
		return;
	}
	XGpioPs_SetDirectionPin(&ButtonGpio, USER_BUTTON_MIO_PIN, 0);
	ButtonGpioReady = true;
}

bool UserButton_pressed(void) {
	UserButtonInit();
	if (!ButtonGpioReady) return false;
	return XGpioPs_ReadPin(&ButtonGpio, USER_BUTTON_MIO_PIN) == 0;
}

/* USR button held at start-up: reset the network settings to DHCP */
static void CheckUserButtonAtBoot(void) {
	if (!UserButton_pressed()) {
		xil_printf("USR button not pressed\r\n");
		return;
	}
	xil_printf("USR button pressed at start-up: keep it pressed for %d ms to reset the network settings to DHCP\r\n", USER_BUTTON_HOLD_MS);
	SetLED0Color(1, 1, 0);
	XTime start;
	XTime_GetTime(&start);
	while (ElapsedMs(start) < USER_BUTTON_HOLD_MS) {
		if (!UserButton_pressed()) {
			WaitMs(20);   /* debounce */
			if (!UserButton_pressed()) {
				xil_printf("USR button released too early, network settings unchanged\r\n");
				SetLED0Color(0, 0, 0);
				return;
			}
		}
	}
	if (Stored.network_mode == NETWORK_MODE_DHCP) {
		xil_printf("USR button: network settings are already DHCP\r\n");
		ButtonResetAtBoot = true;
	} else {
		SettingsRecord record = Stored;   /* keeps the other (future) settings */
		record.network_mode = NETWORK_MODE_DHCP;
		record.ip = record.netmask = record.gateway = 0;
		char error[120];
		if (WriteSettings(&record, error, sizeof(error))) {
			xil_printf("USR button: network settings reset to DHCP\r\n");
			ButtonResetAtBoot = true;
		} else {
			xil_printf("USR button: resetting the network settings failed: %s\r\n", error);
			SetLED0Color(1, 0, 0);
			Beep(3, 700, 300);
			return;
		}
	}
	SetLED0Color(0, 1, 0);
	Beep(2, 150, 150);
}

/* ---------------- start-up ---------------- */

void NetworkSettings_init(void) {
	ReadStoredSettings();
	CheckUserButtonAtBoot();
	Active = Stored;
	char text[120];
	DescribeRecord(text, sizeof(text), &Active);
	xil_printf("network settings: %s%s\r\n", text, (StoredCopy < 0) ? " (default, none stored)" : "");
}

bool NetworkSettings_use_fixed_ip(ip4_addr_t* ip, ip4_addr_t* netmask, ip4_addr_t* gateway) {
	if (Active.network_mode != NETWORK_MODE_FIXED) return false;
	ip->addr = Active.ip;
	netmask->addr = Active.netmask;
	gateway->addr = Active.gateway;
	return true;
}

void NetworkSettings_link_local_address(ip4_addr_t* ip, ip4_addr_t* netmask, ip4_addr_t* gateway) {
	unsigned char mac[6];
	SequencerIdentity_get_mac(mac);
	u32 hash = FirmwareUpdate_crc32(mac, sizeof(mac));
	/* RFC 3927: 169.254.1.0 ... 169.254.254.255 */
	IP4_ADDR(ip, 169, 254, 1 + ((hash >> 8) % 254), hash & 0xFF);
	IP4_ADDR(netmask, 255, 255, 0, 0);
	IP4_ADDR(gateway, 0, 0, 0, 0);
}

static const char* IpSource(void) {
	if (Active.network_mode == NETWORK_MODE_FIXED) return "fixed";
	if (echo_netif == NULL) return "none";
	if (dhcp_supplied_address(echo_netif)) return "DHCP";
	const ip4_addr_t* ip = netif_ip4_addr(echo_netif);
	if (ip4_addr_isany(ip)) return "none (waiting for DHCP)";
	if ((lwip_ntohl(ip->addr) >> 16) == 0xA9FE) return "link-local (no DHCP server)";
	return "other";
}

void NetworkSettings_describe_current(char* text, size_t size) {
	char netmask[16] = "-", gateway[16] = "-";
	if (echo_netif != NULL) {
		FormatIp(netmask, sizeof(netmask), netif_ip4_netmask(echo_netif)->addr);
		FormatIp(gateway, sizeof(gateway), netif_ip4_gw(echo_netif)->addr);
	}
	snprintf(text, size, "netmask %s; gateway %s; IP source %s%s", netmask, gateway, IpSource(),
			ButtonResetAtBoot ? "; network settings reset by USR button at start-up" : "");
}

/* ---------------- TCP commands ---------------- */

static void SendLine(const char* text) {
	/* only in debug mode: the UART at 115200 baud blocks for about 87 us per character, which would delay the answer */
	if (DebugModeOn) xil_printf("%s\r\n", text);
	server_send_ascii((char*)text);
	server_send_ascii("\n");
	if (last_tpcb) tcp_output(last_tpcb);
}

void NetworkSettings_get_command(void) {
	char stored[100], current[160], line[320];
	char ip[16] = "-";
	DescribeRecord(stored, sizeof(stored), &Stored);
	if (echo_netif != NULL) FormatIp(ip, sizeof(ip), netif_ip4_addr(echo_netif)->addr);
	NetworkSettings_describe_current(current, sizeof(current));
	snprintf(line, sizeof(line), "OK stored setting %s%s; current IP %s; %s", stored,
			SameNetworkSettings(&Stored, &Active) ? "" : " (takes effect after the next reboot)", ip, current);
	SendLine(line);
}

void NetworkSettings_set_command(bool sequence_running) {
	char line[200], error[120], text[100];
	unsigned char* settings = server_get_command(/*timeout*/ 10.0);
	if (settings == NULL) {
		SendLine("ERROR set_network_settings: settings not received");
		return;
	}
	if (sequence_running) {
		SendLine("ERROR set_network_settings: a sequence is running");
		return;
	}
	SettingsRecord record = Stored;
	if (!ParseSettings((const char*)settings, &record, error, sizeof(error)) || !WriteSettings(&record, error, sizeof(error))) {
		snprintf(line, sizeof(line), "ERROR set_network_settings: %s", error);
		SendLine(line);
		return;
	}
	DescribeRecord(text, sizeof(text), &Stored);
	snprintf(line, sizeof(line), "OK stored %s; takes effect after the next reboot", text);
	SendLine(line);
}

/* ---------------- UDP command ---------------- */

static void UdpReply(const ip_addr_t* addr, u16_t port, const char* result) {
	unsigned char mac[6];
	char reply[300];
	SequencerIdentity_get_mac(mac);
	snprintf(reply, sizeof(reply), "OpticsFoundry set_network; MAC %02X:%02X:%02X:%02X:%02X:%02X; %s",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], result);
	xil_printf("%s\r\n", reply);
	SequencerIdentity_udp_reply(addr, port, reply);
}

/* parses "XX:XX:XX:XX:XX:XX" (or with '-'); advances *p */
static bool ParseMac(const char** p, unsigned char mac[6]) {
	const char* s = SkipSpaces(*p);
	for (int i = 0; i < 6; i++) {
		if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return false;
		char hex[3] = { s[0], s[1], 0 };
		mac[i] = (unsigned char)strtoul(hex, NULL, 16);
		s += 2;
		if (i < 5) {
			if ((*s != ':') && (*s != '-')) return false;
			s++;
		}
	}
	*p = s;
	return true;
}

#define UDP_SET_NETWORK_REQUEST "OpticsFoundry_set_network"

bool NetworkSettings_handle_udp_request(const char* request, const ip_addr_t* addr, u16_t port) {
	const char* p = request;
	if (strncmp(p, UDP_SET_NETWORK_REQUEST, strlen(UDP_SET_NETWORK_REQUEST)) != 0) return false;
	p += strlen(UDP_SET_NETWORK_REQUEST);
	if ((*p != ' ') && (*p != '\t')) return false;
	unsigned char mac[6], own_mac[6];
	if (!ParseMac(&p, mac)) return true;   /* can't tell whether it was meant for us: no answer */
	SequencerIdentity_get_mac(own_mac);
	if (memcmp(mac, own_mac, sizeof(mac)) != 0) return true;   /* for another sequencer */
	char error[120], result[200];
	SettingsRecord record = Stored;
	if (!ParseSettings(p, &record, error, sizeof(error))) {
		snprintf(result, sizeof(result), "ERROR %s", error);
		UdpReply(addr, port, result);
		return true;
	}
	if (Sequence_running) {
		UdpReply(addr, port, "ERROR a sequence is running");
		return true;
	}
	/* the PC sends the request on all its network interfaces, so further copies may arrive: ignore them */
	if (RebootPending) return true;
	/* Writing the flash here, in the lwIP receive callback, blocks the network for typically 50 ms. If this callback runs
	 * while another flash operation services the network (e.g. a firmware update), the write is refused. */
	char text[100];
	if (!WriteSettings(&record, error, sizeof(error))) {
		snprintf(result, sizeof(result), "ERROR %s", error);
		UdpReply(addr, port, result);
		return true;
	}
	DescribeRecord(text, sizeof(text), &Stored);
	snprintf(result, sizeof(result), "OK stored %s; rebooting", text);
	UdpReply(addr, port, result);
	RebootPending = true;   /* done by NetworkSettings_poll, outside of the receive callback */
	return true;
}

/* ---------------- event loop ---------------- */

void NetworkSettings_poll(void) {
	if (RebootPending && !Rebooting) {
		Rebooting = true;
		FirmwareUpdate_reboot();   /* services the network for a while first, so that the answer goes out */
	}
	/* DHCP mode: if the board has no address (DHCP lease lost) for a while, use the link-local address again.
	 * DHCP continues in the background and replaces it when a server answers. */
	if ((Active.network_mode == NETWORK_MODE_DHCP) && (echo_netif != NULL)) {
		if (!ip4_addr_isany(netif_ip4_addr(echo_netif))) {
			NoAddressSince = 0;
		} else if (NoAddressSince == 0) {
			XTime_GetTime(&NoAddressSince);
		} else if (ElapsedMs(NoAddressSince) > LINK_LOCAL_DELAY_MS) {
			ip4_addr_t ip, netmask, gateway;
			NetworkSettings_link_local_address(&ip, &netmask, &gateway);
			netif_set_addr(echo_netif, &ip, &netmask, &gateway);
			xil_printf("no DHCP address: using link-local address %d.%d.%d.%d\r\n", ip4_addr1(&ip), ip4_addr2(&ip), ip4_addr3(&ip), ip4_addr4(&ip));
			NoAddressSince = 0;
		}
	}
}
