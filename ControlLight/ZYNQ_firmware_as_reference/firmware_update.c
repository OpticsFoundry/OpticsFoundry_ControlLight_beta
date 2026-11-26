/*
 * Firmware update over Ethernet (2026-11-26), see firmware_update.h for the protocol.
 *
 * The new BOOT.BIN is received into DDR, checked (size, CRC32, Zynq boot header),
 * then written to QSPI flash offset 0 and read back for verification.
 * The running application executes from DDR, so the QSPI flash can be rewritten
 * while the firmware runs, also when it was booted from that QSPI flash.
 *
 * Recovery if a QSPI update ever goes wrong (power loss during programming, bad image):
 * boot from SD card (boot mode jumpers) or program the flash via JTAG.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "xparameters.h"
#include "xqspips.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "lwip/tcp.h"
#include "echo.h"
#include "firmware_update.h"

extern bool DebugModeOn;
extern void RunEventLoop();
extern void server_set_ascii_mode();
extern struct tcp_pcb *last_tpcb;

#define FIRMWARE_BUILD __DATE__ " " __TIME__

/* SPI NOR flash commands, common to Winbond W25Q, Micron N25Q, Spansion S25FL, Macronix MX25L, ISSI IS25LP */
#define FLASH_CMD_PAGE_PROGRAM   0x02
#define FLASH_CMD_READ           0x03
#define FLASH_CMD_READ_STATUS    0x05
#define FLASH_CMD_WRITE_ENABLE   0x06
#define FLASH_CMD_SECTOR_ERASE   0xD8   /* 64 KB sector */
#define FLASH_CMD_READ_ID        0x9F

#define FLASH_STATUS_BUSY        0x01
#define FLASH_STATUS_BP_MASK     0x1C   /* block protect bits BP0..BP2 */

#define FLASH_PAGE_SIZE          256
#define FLASH_SECTOR_SIZE        0x10000
#define FLASH_CMD_OVERHEAD       4      /* command byte + 3 address bytes */
#define FLASH_READ_CHUNK         4096
#define FLASH_MAX_SIZE_3BYTE     0x1000000  /* 16 MB, limit of 3-byte addressing */

#define SECTOR_ERASE_TIMEOUT_MS  5000
#define PAGE_PROGRAM_TIMEOUT_MS  50
#define RECEIVE_TIMEOUT_MS       120000

/* Zynq-7000 BootROM header (UG585) */
#define BOOT_HEADER_WIDTH_DETECTION_OFFSET 0x20
#define BOOT_HEADER_WIDTH_DETECTION_WORD   0xAA995566
#define BOOT_HEADER_IMAGE_ID_OFFSET        0x24
#define BOOT_HEADER_IMAGE_ID_WORD          0x584C4E58  /* "XNLX" */
#define BOOT_HEADER_CHECKSUM_OFFSET        0x48        /* ~(sum of words 0x20..0x44) */
#define BOOT_HEADER_MIN_SIZE               0x8C0

/* System level control registers (UG585) */
#define SLCR_UNLOCK_ADDR     0xF8000008
#define SLCR_UNLOCK_KEY      0xDF0D
#define SLCR_PSS_RST_CTRL    0xF8000200
#define SLCR_BOOT_MODE       0xF800025C
#define BOOT_MODE_MASK       0x7
#define BOOT_MODE_JTAG       0x0
#define BOOT_MODE_QSPI       0x1
#define BOOT_MODE_SD         0x5

static XQspiPs Qspi;
static bool QspiReady = false;
static u8 QspiTx[FLASH_CMD_OVERHEAD + FLASH_READ_CHUNK];
static u8 QspiRx[FLASH_CMD_OVERHEAD + FLASH_READ_CHUNK];

static u32 ElapsedMs(XTime start) {
	XTime now;
	XTime_GetTime(&now);
	return (u32)((now - start) / (COUNTS_PER_SECOND / 1000));
}

static void WaitMsServicingNetwork(u32 ms) {
	XTime start;
	XTime_GetTime(&start);
	while (ElapsedMs(start) < ms) RunEventLoop();
}

/* send one line of text to the PC and push it out immediately */
static void SendLine(const char* format, ...) {
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line) - 2, format, args);
	va_end(args);
	xil_printf("%s\r\n", line);
	strcat(line, "\n");
	server_send_ascii(line);
	if (last_tpcb) tcp_output(last_tpcb);
}

/* ---------------- QSPI flash access ---------------- */

static int QspiInit(void) {
	if (QspiReady) return XST_SUCCESS;
	XQspiPs_Config *config = XQspiPs_LookupConfig(XPAR_XQSPIPS_0_DEVICE_ID);
	if (config == NULL) return XST_FAILURE;
	if (XQspiPs_CfgInitialize(&Qspi, config, config->BaseAddress) != XST_SUCCESS) return XST_FAILURE;
	/* leave linear (memory mapped) mode, in case FSBL left it on after booting from QSPI */
	XQspiPs *qspi = &Qspi;  /* the BSP macro XQspiPs_GetLqspiConfigReg doesn't parenthesize its argument */
	XQspiPs_SetLqspiConfigReg(qspi, XQspiPs_GetLqspiConfigReg(qspi) & ~XQSPIPS_LQSPI_CR_LINEAR_MASK);
	if (XQspiPs_SetOptions(&Qspi, XQSPIPS_MANUAL_START_OPTION | XQSPIPS_FORCE_SSELECT_OPTION | XQSPIPS_HOLD_B_DRIVE_OPTION) != XST_SUCCESS) return XST_FAILURE;
	if (XQspiPs_SetClkPrescaler(&Qspi, XQSPIPS_CLK_PRESCALE_8) != XST_SUCCESS) return XST_FAILURE;
	if (XQspiPs_SetSlaveSelect(&Qspi) != XST_SUCCESS) return XST_FAILURE;
	QspiReady = true;
	return XST_SUCCESS;
}

static u8 FlashReadStatus(void) {
	u8 tx[2] = { FLASH_CMD_READ_STATUS, 0 };
	u8 rx[2] = { 0, 0 };
	XQspiPs_PolledTransfer(&Qspi, tx, rx, sizeof(tx));
	return rx[1];
}

static bool FlashWaitWhileBusy(u32 timeout_ms, bool service_network) {
	XTime start;
	XTime_GetTime(&start);
	while (FlashReadStatus() & FLASH_STATUS_BUSY) {
		if (ElapsedMs(start) > timeout_ms) return false;
		if (service_network) RunEventLoop();
	}
	return true;
}

static void FlashWriteEnable(void) {
	u8 cmd = FLASH_CMD_WRITE_ENABLE;
	XQspiPs_PolledTransfer(&Qspi, &cmd, NULL, 1);
}

static void FlashReadId(u8 id[3]) {
	u8 tx[4] = { FLASH_CMD_READ_ID, 0, 0, 0 };
	u8 rx[4] = { 0, 0, 0, 0 };
	XQspiPs_PolledTransfer(&Qspi, tx, rx, sizeof(tx));
	id[0] = rx[1];
	id[1] = rx[2];
	id[2] = rx[3];
}

/* usable flash size in bytes, 0 if no known flash. The third JEDEC ID byte is log2(size) for the usual vendors. */
static u32 FlashUsableSize(const u8 id[3]) {
	if ((id[2] >= 0x10) && (id[2] < 0x18)) return 1u << id[2];
	if ((id[2] >= 0x18) && (id[2] <= 0x22)) return FLASH_MAX_SIZE_3BYTE; /* we only use 3-byte addressing */
	return 0;
}

static void SetCommandAndAddress(u8* buffer, u8 command, u32 address) {
	buffer[0] = command;
	buffer[1] = (u8)(address >> 16);
	buffer[2] = (u8)(address >> 8);
	buffer[3] = (u8)(address);
}

static bool FlashEraseSector(u32 address) {
	u8 tx[FLASH_CMD_OVERHEAD];
	SetCommandAndAddress(tx, FLASH_CMD_SECTOR_ERASE, address);
	FlashWriteEnable();
	XQspiPs_PolledTransfer(&Qspi, tx, NULL, sizeof(tx));
	return FlashWaitWhileBusy(SECTOR_ERASE_TIMEOUT_MS, /*service_network*/ true);
}

/* address must be page aligned, length <= FLASH_PAGE_SIZE */
static bool FlashProgramPage(u32 address, const u8* data, u32 length) {
	SetCommandAndAddress(QspiTx, FLASH_CMD_PAGE_PROGRAM, address);
	memcpy(&QspiTx[FLASH_CMD_OVERHEAD], data, length);
	FlashWriteEnable();
	XQspiPs_PolledTransfer(&Qspi, QspiTx, NULL, length + FLASH_CMD_OVERHEAD);
	return FlashWaitWhileBusy(PAGE_PROGRAM_TIMEOUT_MS, /*service_network*/ false);
}

/* length <= FLASH_READ_CHUNK; returns pointer to the data read */
static const u8* FlashRead(u32 address, u32 length) {
	memset(QspiTx, 0, length + FLASH_CMD_OVERHEAD);
	SetCommandAndAddress(QspiTx, FLASH_CMD_READ, address);
	XQspiPs_PolledTransfer(&Qspi, QspiTx, QspiRx, length + FLASH_CMD_OVERHEAD);
	return &QspiRx[FLASH_CMD_OVERHEAD];
}

/* ---------------- image checks ---------------- */

static u32 Crc32(const u8* data, u32 length) {  /* zlib / IEEE 802.3 CRC32 */
	static u32 table[256];
	static bool table_ready = false;
	if (!table_ready) {
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		table_ready = true;
	}
	u32 crc = 0xFFFFFFFFu;
	for (u32 i = 0; i < length; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

static u32 ReadLE32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool ValidateBootImage(const u8* image, u32 size, char* error, size_t error_size) {
	if (size < BOOT_HEADER_MIN_SIZE) {
		snprintf(error, error_size, "image too small (%lu bytes) to be a Zynq boot image", (unsigned long)size);
		return false;
	}
	if (ReadLE32(image + BOOT_HEADER_WIDTH_DETECTION_OFFSET) != BOOT_HEADER_WIDTH_DETECTION_WORD) {
		snprintf(error, error_size, "no Zynq boot header (width detection word missing). Is this a BOOT.BIN?");
		return false;
	}
	if (ReadLE32(image + BOOT_HEADER_IMAGE_ID_OFFSET) != BOOT_HEADER_IMAGE_ID_WORD) {
		snprintf(error, error_size, "no Zynq boot header (image identification \"XNLX\" missing). Is this a BOOT.BIN?");
		return false;
	}
	u32 sum = 0;
	for (u32 offset = BOOT_HEADER_WIDTH_DETECTION_OFFSET; offset < BOOT_HEADER_CHECKSUM_OFFSET; offset += 4) sum += ReadLE32(image + offset);
	if ((~sum) != ReadLE32(image + BOOT_HEADER_CHECKSUM_OFFSET)) {
		snprintf(error, error_size, "boot header checksum wrong");
		return false;
	}
	return true;
}

static bool IsErased(const u8* data, u32 length) {
	for (u32 i = 0; i < length; i++) if (data[i] != 0xFF) return false;
	return true;
}

/* ---------------- Ethernet command handlers ---------------- */

void FirmwareUpdate_send_firmware_info(void) {
	u8 id[3] = { 0, 0, 0 };
	if (QspiInit() == XST_SUCCESS) FlashReadId(id);
	u32 boot_mode = Xil_In32(SLCR_BOOT_MODE) & BOOT_MODE_MASK;
	const char* boot_mode_name = (boot_mode == BOOT_MODE_QSPI) ? "QSPI" : (boot_mode == BOOT_MODE_SD) ? "SD card" : (boot_mode == BOOT_MODE_JTAG) ? "JTAG" : "other";
	SendLine("OpticsFoundry Z-turn sequencer firmware; build %s; boot mode %s; QSPI flash JEDEC ID %02X %02X %02X; supports update_firmware_qspi, reboot_system",
			FIRMWARE_BUILD, boot_mode_name, id[0], id[1], id[2]);
}

void FirmwareUpdate_update_qspi(u8* buffer, u32 buffer_size, bool sequence_running) {
	char error[160];
	unsigned char* text = server_get_command(/*timeout*/ 10.0);
	if (text == NULL) {
		SendLine("ERROR update_firmware_qspi: image size not received");
		return;
	}
	u32 size = strtoul((char*)text, NULL, 10);
	text = server_get_command(/*timeout*/ 10.0);
	if (text == NULL) {
		SendLine("ERROR update_firmware_qspi: CRC32 not received");
		return;
	}
	u32 expected_crc = strtoul((char*)text, NULL, 10);

	if (sequence_running) {
		SendLine("ERROR update_firmware_qspi: a sequence is running");
		return;
	}
	if (QspiInit() != XST_SUCCESS) {
		SendLine("ERROR update_firmware_qspi: QSPI controller initialization failed");
		return;
	}
	u8 id[3];
	FlashReadId(id);
	u32 flash_size = FlashUsableSize(id);
	if (flash_size == 0) {
		SendLine("ERROR update_firmware_qspi: no supported QSPI flash found (JEDEC ID %02X %02X %02X)", id[0], id[1], id[2]);
		return;
	}
	u8 status = FlashReadStatus();
	if (status & FLASH_STATUS_BP_MASK) {
		SendLine("ERROR update_firmware_qspi: QSPI flash is write protected (status register 0x%02X)", status);
		return;
	}
	if ((size == 0) || (size > buffer_size) || (size > flash_size)) {
		SendLine("ERROR update_firmware_qspi: invalid image size %lu bytes (maximum %lu)", (unsigned long)size,
				(unsigned long)((buffer_size < flash_size) ? buffer_size : flash_size));
		return;
	}

	/* receive image; server_set_binary_mode answers "Ready" */
	xil_printf("update_firmware_qspi: receiving %lu bytes\r\n", (unsigned long)size);
	server_set_binary_mode(buffer, buffer_size, size);
	if (last_tpcb) tcp_output(last_tpcb);
	XTime start;
	XTime_GetTime(&start);
	while (!server_last_digital_data_transfer_successful()) {
		if (ElapsedMs(start) > RECEIVE_TIMEOUT_MS) {
			server_set_ascii_mode();
			SendLine("ERROR update_firmware_qspi: timeout while receiving the image");
			return;
		}
		RunEventLoop();
	}
	u32 crc = Crc32(buffer, size);
	if (crc != expected_crc) {
		SendLine("ERROR update_firmware_qspi: CRC32 mismatch (received data 0x%08lX, expected 0x%08lX)", (unsigned long)crc, (unsigned long)expected_crc);
		return;
	}
	if (!ValidateBootImage(buffer, size, error, sizeof(error))) {
		SendLine("ERROR update_firmware_qspi: %s", error);
		return;
	}
	SendLine("OK received %lu bytes, CRC32 0x%08lX ok, boot header ok; QSPI flash ID %02X %02X %02X; erasing, programming and verifying",
			(unsigned long)size, (unsigned long)crc, id[0], id[1], id[2]);

	/* erase */
	XTime t0;
	XTime_GetTime(&t0);
	u32 sectors = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
	for (u32 sector = 0; sector < sectors; sector++) {
		if (!FlashEraseSector(sector * FLASH_SECTOR_SIZE)) {
			SendLine("ERROR update_firmware_qspi: erasing sector at 0x%06lX timed out. QSPI boot image is now incomplete; boot from SD card or use JTAG to recover",
					(unsigned long)(sector * FLASH_SECTOR_SIZE));
			return;
		}
	}
	u32 erase_ms = ElapsedMs(t0);
	xil_printf("update_firmware_qspi: erased %lu sectors in %lu ms\r\n", (unsigned long)sectors, (unsigned long)erase_ms);

	/* program */
	XTime_GetTime(&t0);
	for (u32 address = 0; address < size; address += FLASH_PAGE_SIZE) {
		u32 length = ((size - address) < FLASH_PAGE_SIZE) ? (size - address) : FLASH_PAGE_SIZE;
		if (!IsErased(buffer + address, length)) {
			if (!FlashProgramPage(address, buffer + address, length)) {
				SendLine("ERROR update_firmware_qspi: programming page at 0x%06lX timed out. QSPI boot image is now incomplete; boot from SD card or use JTAG to recover",
						(unsigned long)address);
				return;
			}
		}
		if ((address % FLASH_SECTOR_SIZE) == 0) RunEventLoop();
	}
	u32 program_ms = ElapsedMs(t0);
	xil_printf("update_firmware_qspi: programmed in %lu ms\r\n", (unsigned long)program_ms);

	/* verify */
	XTime_GetTime(&t0);
	for (u32 address = 0; address < size; address += FLASH_READ_CHUNK) {
		u32 length = ((size - address) < FLASH_READ_CHUNK) ? (size - address) : FLASH_READ_CHUNK;
		const u8* data = FlashRead(address, length);
		if (memcmp(data, buffer + address, length) != 0) {
			u32 i = 0;
			while ((i < length) && (data[i] == buffer[address + i])) i++;
			SendLine("ERROR update_firmware_qspi: verify failed at flash address 0x%06lX (read 0x%02X, expected 0x%02X). QSPI boot image is corrupt; boot from SD card or use JTAG to recover",
					(unsigned long)(address + i), data[i], buffer[address + i]);
			return;
		}
		if ((address % FLASH_SECTOR_SIZE) == 0) RunEventLoop();
	}
	u32 verify_ms = ElapsedMs(t0);
	SendLine("OK programmed and verified %lu bytes at QSPI flash offset 0 (erase %lu ms, program %lu ms, verify %lu ms)",
			(unsigned long)size, (unsigned long)erase_ms, (unsigned long)program_ms, (unsigned long)verify_ms);
}

void FirmwareUpdate_reboot(void) {
	SendLine("OK rebooting");
	WaitMsServicingNetwork(200);   /* let lwIP transmit the answer */
	if (last_tpcb) {
		tcp_recv(last_tpcb, NULL);
		tcp_close(last_tpcb);
		last_tpcb = NULL;
	}
	WaitMsServicingNetwork(100);
	xil_printf("\r\nreboot_system: software reset\r\n");
	XTime start;
	XTime_GetTime(&start);
	while (ElapsedMs(start) < 20);  /* let the UART FIFO drain */
	Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY);
	Xil_Out32(SLCR_PSS_RST_CTRL, 1);  /* SOFT_RST: resets PS and PL, BootROM boots again */
	while (1);
}
