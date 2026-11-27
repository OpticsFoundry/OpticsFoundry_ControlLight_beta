/*
 * Firmware update over Ethernet and backup boot image (2026-11-26), see firmware_update.h.
 *
 * A new BOOT.BIN is received into DDR, checked (size, CRC32, Zynq boot header), then written
 * to one of the two image slots of the QSPI flash and read back for verification. The first
 * flash page of the slot, which holds the boot header, is written last. The running application
 * executes from DDR, so the flash can be rewritten while the firmware runs, also when it was
 * booted from that flash.
 *
 * Recovery if both QSPI images are unusable: boot from SD card (boot mode jumpers) or use JTAG.
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
#include "sequencer_identity.h"
#include "sd_installer.h"
#include "network_settings.h"
#include "trial_boot.h"

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
#define FLASH_CMD_SUBSECTOR_ERASE 0x20  /* 4 KB subsector */
#define FLASH_CMD_READ_ID        0x9F
#define FLASH_CMD_READ_UNIQUE_ID 0x4B   /* Winbond: 4 dummy bytes, then 64 bit unique ID */

#define FLASH_MANUFACTURER_WINBOND 0xEF

#define FLASH_STATUS_BUSY        0x01
#define FLASH_STATUS_BP_MASK     0x1C   /* block protect bits BP0..BP2 */

#define FLASH_PAGE_SIZE          256
#define FLASH_SECTOR_SIZE        0x10000
#define FLASH_CMD_OVERHEAD       4      /* command byte + 3 address bytes */
#define FLASH_READ_CHUNK         4096
#define FLASH_MAX_SIZE_3BYTE     0x1000000  /* 16 MB, limit of 3-byte addressing */

#define PRIMARY_IMAGE_MAX_SIZE   (QSPI_BACKUP_IMAGE_OFFSET - QSPI_PRIMARY_IMAGE_OFFSET)
#define BACKUP_IMAGE_MAX_SIZE    (QSPI_SETTINGS_OFFSET - QSPI_BACKUP_IMAGE_OFFSET)

#define SECTOR_ERASE_TIMEOUT_MS  5000
#define SUBSECTOR_ERASE_TIMEOUT_MS 1000
#define FLASH_SUBSECTOR_SIZE     0x1000
#define PAGE_PROGRAM_TIMEOUT_MS  50
#define RECEIVE_TIMEOUT_MS       120000

/* Zynq-7000 BootROM header (UG585) */
#define BOOT_HEADER_WIDTH_DETECTION_OFFSET 0x20
#define BOOT_HEADER_WIDTH_DETECTION_WORD   0xAA995566
#define BOOT_HEADER_IMAGE_ID_OFFSET        0x24
#define BOOT_HEADER_IMAGE_ID_WORD          0x584C4E58  /* "XNLX" */
#define BOOT_HEADER_CHECKSUM_OFFSET        0x48        /* ~(sum of words 0x20..0x44) */
#define BOOT_HEADER_CHECK_SIZE             (BOOT_HEADER_CHECKSUM_OFFSET + 4)
#define BOOT_IMAGE_MIN_SIZE                0x8C0

/* System level control and device configuration registers (UG585) */
#define SLCR_UNLOCK_ADDR     0xF8000008
#define SLCR_UNLOCK_KEY      0xDF0D
#define SLCR_PSS_RST_CTRL    0xF8000200
#define SLCR_BOOT_MODE       0xF800025C
#define BOOT_MODE_MASK       0x7
#define BOOT_MODE_JTAG       0x0
#define BOOT_MODE_QSPI       0x1
#define BOOT_MODE_SD         0x5
#define DEVCFG_MULTIBOOT     0xF800702C  /* image start = (MULTIBOOT & 0x1FFF) * 32 KB, set by BootROM, kept over software resets */
#define DEVCFG_UNLOCK        0xF8007034
#define DEVCFG_UNLOCK_KEY    0x757BDF0D
#define MULTIBOOT_OFFSET_MASK 0x1FFF
#define MULTIBOOT_UNIT       0x8000

static XQspiPs Qspi;
static bool QspiReady = false;
static u8 QspiTx[FLASH_CMD_OVERHEAD + FLASH_READ_CHUNK];
static u8 QspiRx[FLASH_CMD_OVERHEAD + FLASH_READ_CHUNK];

/* cached at start-up (FirmwareUpdate_init) and after flash writes, so that status requests never access the flash */
static bool Initialized = false;
static u8 FlashJedecId[3] = { 0, 0, 0 };
static u8 FlashUniqueId[8];
static bool FlashUniqueIdValid = false;
static bool PrimaryImageValid = false;
static bool BackupImageValid = false;
static bool FlashOperationInProgress = false;
/* false while installing from SD card at start-up, before the network exists: reports only go to the UART */
static bool UseNetwork = true;

/* called once per 64 KB flash block while erasing, programming, verifying and comparing images, see firmware_update.h */
static void (*ProgressCallback)(void) = NULL;

void FirmwareUpdate_set_progress_callback(void (*callback)(void)) {
	ProgressCallback = callback;
}

static void Progress(void) {
	if (ProgressCallback) ProgressCallback();
}

static void ServiceNetwork(void) {
	if (UseNetwork) RunEventLoop();
}

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
static void SendLineV(const char* format, va_list args) {
	char line[1000];
	vsnprintf(line, sizeof(line) - 2, format, args);
	xil_printf("%s\r\n", line);
	if (!UseNetwork) return;
	strcat(line, "\n");
	server_send_ascii(line);
	if (last_tpcb) tcp_output(last_tpcb);
}

static void SendLine(const char* format, ...) {
	va_list args;
	va_start(args, format);
	SendLineV(format, args);
	va_end(args);
}

void FirmwareUpdate_send_line(const char* format, ...) {
	va_list args;
	va_start(args, format);
	SendLineV(format, args);
	va_end(args);
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
		if (service_network) ServiceNetwork();
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

static bool FlashEraseSubsector(u32 address) {
	u8 tx[FLASH_CMD_OVERHEAD];
	SetCommandAndAddress(tx, FLASH_CMD_SUBSECTOR_ERASE, address);
	FlashWriteEnable();
	XQspiPs_PolledTransfer(&Qspi, tx, NULL, sizeof(tx));
	return FlashWaitWhileBusy(SUBSECTOR_ERASE_TIMEOUT_MS, /*service_network*/ false);
}

/* length <= FLASH_READ_CHUNK; returns pointer to the data read */
static const u8* FlashRead(u32 address, u32 length) {
	memset(QspiTx, 0, length + FLASH_CMD_OVERHEAD);
	SetCommandAndAddress(QspiTx, FLASH_CMD_READ, address);
	XQspiPs_PolledTransfer(&Qspi, QspiTx, QspiRx, length + FLASH_CMD_OVERHEAD);
	return &QspiRx[FLASH_CMD_OVERHEAD];
}

static bool FlashReadUniqueId(u8 unique_id[8]) {
	u8 tx[13];
	u8 rx[13];
	memset(tx, 0, sizeof(tx));
	memset(rx, 0, sizeof(rx));
	tx[0] = FLASH_CMD_READ_UNIQUE_ID;
	XQspiPs_PolledTransfer(&Qspi, tx, rx, sizeof(tx));
	memcpy(unique_id, &rx[5], 8);
	bool all_zero = true, all_ones = true;
	for (int i = 0; i < 8; i++) {
		if (unique_id[i] != 0x00) all_zero = false;
		if (unique_id[i] != 0xFF) all_ones = false;
	}
	return !(all_zero || all_ones);
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

u32 FirmwareUpdate_crc32(const u8* data, u32 length) {
	return Crc32(data, length);
}

static u32 ReadLE32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* header points to at least BOOT_HEADER_CHECK_SIZE bytes */
static bool ValidateBootHeader(const u8* header, char* error, size_t error_size) {
	if (ReadLE32(header + BOOT_HEADER_WIDTH_DETECTION_OFFSET) != BOOT_HEADER_WIDTH_DETECTION_WORD) {
		snprintf(error, error_size, "no Zynq boot header (width detection word missing). Is this a BOOT.BIN?");
		return false;
	}
	if (ReadLE32(header + BOOT_HEADER_IMAGE_ID_OFFSET) != BOOT_HEADER_IMAGE_ID_WORD) {
		snprintf(error, error_size, "no Zynq boot header (image identification \"XNLX\" missing). Is this a BOOT.BIN?");
		return false;
	}
	u32 sum = 0;
	for (u32 offset = BOOT_HEADER_WIDTH_DETECTION_OFFSET; offset < BOOT_HEADER_CHECKSUM_OFFSET; offset += 4) sum += ReadLE32(header + offset);
	if ((~sum) != ReadLE32(header + BOOT_HEADER_CHECKSUM_OFFSET)) {
		snprintf(error, error_size, "boot header checksum wrong");
		return false;
	}
	return true;
}

static bool FlashSlotHasValidBootHeader(u32 slot_offset) {
	char error[100];
	return ValidateBootHeader(FlashRead(slot_offset, BOOT_HEADER_CHECK_SIZE), error, sizeof(error));
}

static void UpdateCachedSlotStatus(void) {
	PrimaryImageValid = FlashSlotHasValidBootHeader(QSPI_PRIMARY_IMAGE_OFFSET);
	BackupImageValid = FlashSlotHasValidBootHeader(QSPI_BACKUP_IMAGE_OFFSET);
}

static bool IsErased(const u8* data, u32 length) {
	for (u32 i = 0; i < length; i++) if (data[i] != 0xFF) return false;
	return true;
}

/* ---------------- status ---------------- */

void FirmwareUpdate_init(void) {
	if (Initialized) return;
	Initialized = true;
	if (QspiInit() != XST_SUCCESS) {
		xil_printf("FirmwareUpdate_init: QSPI initialization failed\r\n");
		return;
	}
	FlashReadId(FlashJedecId);
	if (FlashJedecId[0] == FLASH_MANUFACTURER_WINBOND) FlashUniqueIdValid = FlashReadUniqueId(FlashUniqueId);
	if (FlashUsableSize(FlashJedecId) >= FLASH_MAX_SIZE_3BYTE) UpdateCachedSlotStatus();
}

bool FirmwareUpdate_read_flash_jedec_id(u8 id[3]) {
	FirmwareUpdate_init();
	memcpy(id, FlashJedecId, 3);
	return FlashUsableSize(FlashJedecId) > 0;
}

bool FirmwareUpdate_read_flash_unique_id(u8 unique_id[8]) {
	FirmwareUpdate_init();
	if (!FlashUniqueIdValid) return false;
	memcpy(unique_id, FlashUniqueId, 8);
	return true;
}

static const char* BootModeName(void) {
	u32 boot_mode = Xil_In32(SLCR_BOOT_MODE) & BOOT_MODE_MASK;
	return (boot_mode == BOOT_MODE_QSPI) ? "QSPI" : (boot_mode == BOOT_MODE_SD) ? "SD card" : (boot_mode == BOOT_MODE_JTAG) ? "JTAG" : "other";
}

/* offset of the QSPI image the BootROM booted, or 0xFFFFFFFF if not booted from QSPI */
static u32 BootedImageOffset(void) {
	if ((Xil_In32(SLCR_BOOT_MODE) & BOOT_MODE_MASK) != BOOT_MODE_QSPI) return 0xFFFFFFFF;
	return (Xil_In32(DEVCFG_MULTIBOOT) & MULTIBOOT_OFFSET_MASK) * MULTIBOOT_UNIT;
}

/* "build ...; boot mode ...; image ...; backup image ...; QSPI flash ID ..." */
void FirmwareUpdate_describe(char* text, size_t size) {
	FirmwareUpdate_init();
	char image[80];
	u32 booted_offset = BootedImageOffset();
	if (booted_offset == 0xFFFFFFFF) snprintf(image, sizeof(image), "-");
	else if (booted_offset == QSPI_PRIMARY_IMAGE_OFFSET) snprintf(image, sizeof(image), "primary (QSPI offset 0x%06lX)", (unsigned long)booted_offset);
	else if (booted_offset == QSPI_BACKUP_IMAGE_OFFSET) snprintf(image, sizeof(image), "BACKUP (QSPI offset 0x%06lX; primary image missing or broken)", (unsigned long)booted_offset);
	else snprintf(image, sizeof(image), "unexpected QSPI offset 0x%06lX", (unsigned long)booted_offset);
	char unique_id[24];
	if (FlashUniqueIdValid) snprintf(unique_id, sizeof(unique_id), "%02X%02X%02X%02X%02X%02X%02X%02X",
			FlashUniqueId[0], FlashUniqueId[1], FlashUniqueId[2], FlashUniqueId[3], FlashUniqueId[4], FlashUniqueId[5], FlashUniqueId[6], FlashUniqueId[7]);
	else snprintf(unique_id, sizeof(unique_id), "none");
	snprintf(text, size, "build %s; boot mode %s; image %s; primary image %s; backup image %s; QSPI flash ID %02X %02X %02X; flash unique ID %s",
			FIRMWARE_BUILD, BootModeName(), image, PrimaryImageValid ? "present" : "MISSING", BackupImageValid ? "present" : "MISSING",
			FlashJedecId[0], FlashJedecId[1], FlashJedecId[2], unique_id);
}

/* ---------------- settings sector (see firmware_update.h) ---------------- */

static bool SettingsAccessOk(u32 offset, u32 length, u32 max_length) {
	FirmwareUpdate_init();
	if (!QspiReady || FlashOperationInProgress) return false;
	if (FlashUsableSize(FlashJedecId) < FLASH_MAX_SIZE_3BYTE) return false;
	if ((length == 0) || (length > max_length)) return false;
	if ((offset >= QSPI_SETTINGS_SIZE) || (length > QSPI_SETTINGS_SIZE - offset)) return false;
	return true;
}

bool FirmwareUpdate_read_settings(u32 offset, u8* data, u32 length) {
	if (!SettingsAccessOk(offset, length, FLASH_READ_CHUNK)) return false;
	memcpy(data, FlashRead(QSPI_SETTINGS_OFFSET + offset, length), length);
	return true;
}

bool FirmwareUpdate_write_settings(u32 offset, const u8* data, u32 length) {
	if ((offset % FLASH_SUBSECTOR_SIZE) != 0) return false;
	if (!SettingsAccessOk(offset, length, FLASH_PAGE_SIZE)) return false;
	if (FlashReadStatus() & FLASH_STATUS_BP_MASK) return false;
	const u32 address = QSPI_SETTINGS_OFFSET + offset;
	FlashOperationInProgress = true;
	bool ok = FlashEraseSubsector(address) && FlashProgramPage(address, data, length);
	ok = ok && (memcmp(FlashRead(address, length), data, length) == 0);
	FlashOperationInProgress = false;
	return ok;
}

bool FirmwareUpdate_erase_settings(u32 offset) {
	if ((offset % FLASH_SUBSECTOR_SIZE) != 0) return false;
	if (!SettingsAccessOk(offset, 1, 1)) return false;
	if (FlashReadStatus() & FLASH_STATUS_BP_MASK) return false;
	const u32 address = QSPI_SETTINGS_OFFSET + offset;
	FlashOperationInProgress = true;
	bool ok = FlashEraseSubsector(address) && IsErased(FlashRead(address, 16), 16);
	FlashOperationInProgress = false;
	return ok;
}

/* ---------------- slot status and invalidation ---------------- */

bool FirmwareUpdate_primary_image_valid(void) {
	FirmwareUpdate_init();
	return PrimaryImageValid;
}

bool FirmwareUpdate_backup_image_valid(void) {
	FirmwareUpdate_init();
	return BackupImageValid;
}

/* erases the first sector of the primary slot, which holds its boot header; the BootROM then boots the backup image */
bool FirmwareUpdate_invalidate_primary_image(bool use_network) {
	FirmwareUpdate_init();
	if (!QspiReady || FlashOperationInProgress) return false;
	if (FlashUsableSize(FlashJedecId) < FLASH_MAX_SIZE_3BYTE) return false;
	if (FlashReadStatus() & FLASH_STATUS_BP_MASK) return false;
	UseNetwork = use_network;
	FlashOperationInProgress = true;
	bool erased = FlashEraseSector(QSPI_PRIMARY_IMAGE_OFFSET);
	UpdateCachedSlotStatus();
	FlashOperationInProgress = false;
	UseNetwork = true;
	return erased && !PrimaryImageValid;
}

/* ---------------- Ethernet command handlers ---------------- */

void FirmwareUpdate_send_firmware_info(void) {
	char firmware[256];
	char identity[300];
	char sd_install[200];
	char trial_boot[300];
	FirmwareUpdate_describe(firmware, sizeof(firmware));
	SequencerIdentity_describe(identity, sizeof(identity));
	SdInstaller_describe(sd_install, sizeof(sd_install));
	TrialBoot_describe(trial_boot, sizeof(trial_boot));
	SendLine("OpticsFoundry Z-turn sequencer firmware; %s; %s%s%s; USR button %s; supports update_firmware_qspi, update_backup_firmware_qspi, test_backup_boot, install_from_sd, read_firmware_image, reboot_system, discovery, interrupt_wait, network_settings, trial_boot",
			firmware, identity, sd_install, trial_boot, UserButton_pressed() ? "pressed" : "released");
}

/* verify flash against data, returns false and sends an error line on mismatch */
static bool VerifyFlash(u32 flash_address, const u8* data, u32 length, const char* recovery_hint) {
	for (u32 done = 0; done < length; done += FLASH_READ_CHUNK) {
		u32 chunk = ((length - done) < FLASH_READ_CHUNK) ? (length - done) : FLASH_READ_CHUNK;
		const u8* flash = FlashRead(flash_address + done, chunk);
		if (memcmp(flash, data + done, chunk) != 0) {
			u32 i = 0;
			while ((i < chunk) && (flash[i] == data[done + i])) i++;
			SendLine("ERROR update_firmware_qspi: verify failed at flash address 0x%06lX (read 0x%02X, expected 0x%02X). %s",
					(unsigned long)(flash_address + done + i), flash[i], data[done + i], recovery_hint);
			return false;
		}
		if (((flash_address + done) % FLASH_SECTOR_SIZE) == 0) {
			Progress();
			ServiceNetwork();
		}
	}
	return true;
}

/*
 * Write image to the slot starting at slot_offset.
 * Order: erase (first sector first, which removes the old boot header at once), program everything
 * except the first page, verify it, then program and verify the first page with the boot header.
 * If this is interrupted, the slot has no valid boot header and the BootROM skips it.
 */
static bool WriteImageToSlot(u32 slot_offset, const u8* image, u32 size, const char* recovery_hint) {
	XTime t0;
	XTime_GetTime(&t0);
	u32 sectors = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
	for (u32 sector = 0; sector < sectors; sector++) {
		Progress();
		if (!FlashEraseSector(slot_offset + sector * FLASH_SECTOR_SIZE)) {
			SendLine("ERROR update_firmware_qspi: erasing sector at 0x%06lX timed out. %s", (unsigned long)(slot_offset + sector * FLASH_SECTOR_SIZE), recovery_hint);
			return false;
		}
	}
	u32 erase_ms = ElapsedMs(t0);
	xil_printf("update_firmware_qspi: erased %lu sectors in %lu ms\r\n", (unsigned long)sectors, (unsigned long)erase_ms);

	XTime_GetTime(&t0);
	for (u32 address = FLASH_PAGE_SIZE; address < size; address += FLASH_PAGE_SIZE) {
		u32 length = ((size - address) < FLASH_PAGE_SIZE) ? (size - address) : FLASH_PAGE_SIZE;
		if (!IsErased(image + address, length)) {
			if (!FlashProgramPage(slot_offset + address, image + address, length)) {
				SendLine("ERROR update_firmware_qspi: programming page at 0x%06lX timed out. %s", (unsigned long)(slot_offset + address), recovery_hint);
				return false;
			}
		}
		if ((address % FLASH_SECTOR_SIZE) == 0) {
			Progress();
			ServiceNetwork();
		}
	}
	u32 program_ms = ElapsedMs(t0);

	XTime_GetTime(&t0);
	if (!VerifyFlash(slot_offset + FLASH_PAGE_SIZE, image + FLASH_PAGE_SIZE, size - FLASH_PAGE_SIZE, recovery_hint)) return false;
	/* now the boot header */
	if (!FlashProgramPage(slot_offset, image, FLASH_PAGE_SIZE)) {
		SendLine("ERROR update_firmware_qspi: programming the boot header at 0x%06lX timed out. %s", (unsigned long)slot_offset, recovery_hint);
		return false;
	}
	if (!VerifyFlash(slot_offset, image, FLASH_PAGE_SIZE, recovery_hint)) return false;
	u32 verify_ms = ElapsedMs(t0);
	SendLine("OK programmed and verified %lu bytes at QSPI flash offset 0x%06lX (erase %lu ms, program %lu ms, verify %lu ms)",
			(unsigned long)size, (unsigned long)slot_offset, (unsigned long)erase_ms, (unsigned long)program_ms, (unsigned long)verify_ms);
	return true;
}

void FirmwareUpdate_update_qspi(u8* buffer, u32 buffer_size, bool sequence_running, bool backup_image) {
	const char* command = backup_image ? "update_backup_firmware_qspi" : "update_firmware_qspi";
	const u32 slot_offset = backup_image ? QSPI_BACKUP_IMAGE_OFFSET : QSPI_PRIMARY_IMAGE_OFFSET;
	const u32 slot_size = backup_image ? BACKUP_IMAGE_MAX_SIZE : PRIMARY_IMAGE_MAX_SIZE;
	char error[160];
	unsigned char* text = server_get_command(/*timeout*/ 10.0);
	if (text == NULL) {
		SendLine("ERROR %s: image size not received", command);
		return;
	}
	u32 size = strtoul((char*)text, NULL, 10);
	text = server_get_command(/*timeout*/ 10.0);
	if (text == NULL) {
		SendLine("ERROR %s: CRC32 not received", command);
		return;
	}
	u32 expected_crc = strtoul((char*)text, NULL, 10);

	if (sequence_running) {
		SendLine("ERROR %s: a sequence is running", command);
		return;
	}
	if (FlashOperationInProgress) {
		SendLine("ERROR %s: another flash operation is in progress", command);
		return;
	}
	if (TrialBoot_running()) {
		/* the trial watchdog would reset the board during the update, and a failed trial falls back to the backup image */
		SendLine("ERROR %s: the firmware runs on trial (trial boot) and hasn't been confirmed yet. Confirm it first (confirm_firmware).", command);
		return;
	}
	FirmwareUpdate_init();
	if (!QspiReady) {
		SendLine("ERROR %s: QSPI controller initialization failed", command);
		return;
	}
	u8 id[3];
	FlashReadId(id);
	if (FlashUsableSize(id) < FLASH_MAX_SIZE_3BYTE) {
		SendLine("ERROR %s: no supported 16 MB QSPI flash found (JEDEC ID %02X %02X %02X)", command, id[0], id[1], id[2]);
		return;
	}
	u8 status = FlashReadStatus();
	if (status & FLASH_STATUS_BP_MASK) {
		SendLine("ERROR %s: QSPI flash is write protected (status register 0x%02X)", command, status);
		return;
	}
	if ((size < BOOT_IMAGE_MIN_SIZE) || (size > buffer_size) || (size > slot_size)) {
		SendLine("ERROR %s: invalid image size %lu bytes (maximum %lu)", command, (unsigned long)size,
				(unsigned long)((buffer_size < slot_size) ? buffer_size : slot_size));
		return;
	}
	if (backup_image) {
		UpdateCachedSlotStatus();
		if (!PrimaryImageValid) {
			/* if writing the backup were interrupted now, there would be no bootable image in the flash */
			SendLine("ERROR %s: there is no valid primary image. Write the primary image first.", command);
			return;
		}
	}

	/* receive image; server_set_binary_mode answers "Ready" */
	xil_printf("%s: receiving %lu bytes\r\n", command, (unsigned long)size);
	server_set_binary_mode(buffer, buffer_size, size);
	if (last_tpcb) tcp_output(last_tpcb);
	XTime start;
	XTime_GetTime(&start);
	while (!server_last_digital_data_transfer_successful()) {
		if (ElapsedMs(start) > RECEIVE_TIMEOUT_MS) {
			server_set_ascii_mode();
			SendLine("ERROR %s: timeout while receiving the image", command);
			return;
		}
		RunEventLoop();
	}
	u32 crc = Crc32(buffer, size);
	if (crc != expected_crc) {
		SendLine("ERROR %s: CRC32 mismatch (received data 0x%08lX, expected 0x%08lX)", command, (unsigned long)crc, (unsigned long)expected_crc);
		return;
	}
	if (!ValidateBootHeader(buffer, error, sizeof(error))) {
		SendLine("ERROR %s: %s", command, error);
		return;
	}
	SendLine("OK received %lu bytes, CRC32 0x%08lX ok, boot header ok; writing %s image; erasing, programming and verifying",
			(unsigned long)size, (unsigned long)crc, backup_image ? "backup" : "primary");

	const char* recovery_hint = backup_image
			? "The backup image is incomplete; the primary image is unchanged. Write the backup image again."
			: (BackupImageValid ? "The primary image is incomplete; the board will boot the backup image. Write the primary image again."
			                    : "The primary image is incomplete and there is no backup image; boot from SD card or use JTAG to recover.");
	FlashOperationInProgress = true;
	bool written = WriteImageToSlot(slot_offset, buffer, size, recovery_hint);
	UpdateCachedSlotStatus();
	FlashOperationInProgress = false;
	if (backup_image) {
		if (written) TrialBoot_backup_image_written(crc);
	} else {
		if (written) TrialBoot_primary_image_written(buffer, size, crc);
		else TrialBoot_primary_image_changed();
	}
}

/* true if the slot already contains exactly this image */
static bool SlotContainsImage(u32 slot_offset, const u8* image, u32 size) {
	for (u32 done = 0; done < size; done += FLASH_READ_CHUNK) {
		u32 chunk = ((size - done) < FLASH_READ_CHUNK) ? (size - done) : FLASH_READ_CHUNK;
		if (memcmp(FlashRead(slot_offset + done, chunk), image + done, chunk) != 0) return false;
		if (((slot_offset + done) % FLASH_SECTOR_SIZE) == 0) {
			Progress();
			ServiceNetwork();
		}
	}
	return true;
}

int FirmwareUpdate_install_image(const u8* image, u32 size, bool use_network, char* summary, size_t summary_size) {
	char error[160];
	int result = FIRMWARE_INSTALL_FAILED;
	snprintf(summary, summary_size, "failed");
	FirmwareUpdate_init();
	if (FlashOperationInProgress || !QspiReady) {
		snprintf(summary, summary_size, "failed: QSPI flash not available");
		return result;
	}
	if (TrialBoot_running()) {
		snprintf(summary, summary_size, "failed: the firmware runs on trial (trial boot) and hasn't been confirmed yet. Confirm it first (confirm_firmware).");
		return result;
	}
	if (FlashUsableSize(FlashJedecId) < FLASH_MAX_SIZE_3BYTE) {
		snprintf(summary, summary_size, "failed: no supported 16 MB QSPI flash (JEDEC ID %02X %02X %02X)", FlashJedecId[0], FlashJedecId[1], FlashJedecId[2]);
		return result;
	}
	u8 status = FlashReadStatus();
	if (status & FLASH_STATUS_BP_MASK) {
		snprintf(summary, summary_size, "failed: QSPI flash is write protected (status register 0x%02X)", status);
		return result;
	}
	if ((size < BOOT_IMAGE_MIN_SIZE) || (size > BACKUP_IMAGE_MAX_SIZE)) {
		snprintf(summary, summary_size, "failed: invalid image size %lu bytes", (unsigned long)size);
		return result;
	}
	if (!ValidateBootHeader(image, error, sizeof(error))) {
		snprintf(summary, summary_size, "failed: %s", error);
		return result;
	}
	UseNetwork = use_network;
	FlashOperationInProgress = true;
	bool written = false;
	bool ok = true;
	/* primary first, so that there is always one complete image if the installation is interrupted */
	const u32 slots[2] = { QSPI_PRIMARY_IMAGE_OFFSET, QSPI_BACKUP_IMAGE_OFFSET };
	const char* names[2] = { "primary", "backup" };
	for (int i = 0; (i < 2) && ok; i++) {
		if (SlotContainsImage(slots[i], image, size)) {
			SendLine("OK %s image is already identical, not written", names[i]);
			continue;
		}
		ok = WriteImageToSlot(slots[i], image, size, "Install the image again.");
		written = written || ok;
	}
	UpdateCachedSlotStatus();
	FlashOperationInProgress = false;
	UseNetwork = true;
	if (written || !ok) TrialBoot_primary_image_changed();
	if (!ok) {
		snprintf(summary, summary_size, "failed while writing the QSPI flash");
		return result;
	}
	snprintf(summary, summary_size, written ? "installed %lu byte image, CRC32 0x%08lX, into both QSPI slots" : "QSPI flash already contained this %lu byte image (CRC32 0x%08lX)",
			(unsigned long)size, (unsigned long)Crc32(image, size));
	return written ? FIRMWARE_INSTALL_WRITTEN : FIRMWARE_INSTALL_UP_TO_DATE;
}

/* length in bytes of the boot image in a slot, from its partition header table; 0 if there is no valid image.
 * The image ends with the end of its last partition (FSBL, bitstream, application). */
#define BOOT_HEADER_PARTITION_TABLE_OFFSET 0x9C
#define PARTITION_HEADER_SIZE 64
#define MAX_PARTITIONS 16
static u32 SlotImageLength(u32 slot_offset) {
	char error[100];
	if (!ValidateBootHeader(FlashRead(slot_offset, BOOT_HEADER_CHECK_SIZE), error, sizeof(error))) return 0;
	u32 table = ReadLE32(FlashRead(slot_offset + BOOT_HEADER_PARTITION_TABLE_OFFSET, 4));
	if ((table < BOOT_HEADER_PARTITION_TABLE_OFFSET + 4) || (table > 0x10000)) return 0;
	u32 end = 0;
	for (int i = 0; i < MAX_PARTITIONS; i++) {
		const u8* partition = FlashRead(slot_offset + table + i * PARTITION_HEADER_SIZE, PARTITION_HEADER_SIZE);
		bool empty = true;
		for (int k = 0; k < PARTITION_HEADER_SIZE - 4; k++) if (partition[k] != 0) empty = false;  /* last word is the checksum */
		if (empty) break;
		u32 total_length_in_words = ReadLE32(partition + 8);
		u32 data_offset_in_words = ReadLE32(partition + 20);
		u32 partition_end = (data_offset_in_words + total_length_in_words) * 4;
		if (partition_end > end) end = partition_end;
	}
	return end;
}

void FirmwareUpdate_send_image(u8* buffer, u32 buffer_size, bool sequence_running) {
	if (sequence_running) {
		SendLine("ERROR read_firmware_image: a sequence is running");
		return;
	}
	FirmwareUpdate_init();
	if (!QspiReady || FlashOperationInProgress) {
		SendLine("ERROR read_firmware_image: QSPI flash not available");
		return;
	}
	/* the image the board booted from QSPI; after booting from SD card or JTAG the primary image (or the backup image if the primary one is invalid) */
	u32 booted = BootedImageOffset();
	u32 slot = (booted == QSPI_BACKUP_IMAGE_OFFSET) ? QSPI_BACKUP_IMAGE_OFFSET : QSPI_PRIMARY_IMAGE_OFFSET;
	u32 size = SlotImageLength(slot);
	if ((size == 0) && (booted == 0xFFFFFFFF) && (slot == QSPI_PRIMARY_IMAGE_OFFSET)) {
		slot = QSPI_BACKUP_IMAGE_OFFSET;
		size = SlotImageLength(slot);
	}
	u32 slot_size = (slot == QSPI_BACKUP_IMAGE_OFFSET) ? BACKUP_IMAGE_MAX_SIZE : PRIMARY_IMAGE_MAX_SIZE;
	if ((size < BOOT_IMAGE_MIN_SIZE) || (size > slot_size) || (size > buffer_size)) {
		SendLine("ERROR read_firmware_image: no valid boot image in the QSPI flash");
		return;
	}
	for (u32 done = 0; done < size; done += FLASH_READ_CHUNK) {
		u32 chunk = ((size - done) < FLASH_READ_CHUNK) ? (size - done) : FLASH_READ_CHUNK;
		memcpy(buffer + done, FlashRead(slot + done, chunk), chunk);
		if ((done % FLASH_SECTOR_SIZE) == 0) ServiceNetwork();
	}
	char error[100];
	if (!ValidateBootHeader(buffer, error, sizeof(error))) {
		SendLine("ERROR read_firmware_image: %s", error);
		return;
	}
	SendLine("OK %s %lu %lu", (slot == QSPI_BACKUP_IMAGE_OFFSET) ? "backup" : "primary", (unsigned long)size, (unsigned long)Crc32(buffer, size));
	server_send_binary((char*)buffer, size, DebugModeOn);
	if (last_tpcb) tcp_output(last_tpcb);
}

void FirmwareUpdate_test_backup_boot(bool sequence_running) {
	if (sequence_running) {
		SendLine("ERROR test_backup_boot: a sequence is running");
		return;
	}
	FirmwareUpdate_init();
	if (!QspiReady || FlashOperationInProgress) {
		SendLine("ERROR test_backup_boot: QSPI flash not available");
		return;
	}
	UpdateCachedSlotStatus();
	if (!BackupImageValid) {
		SendLine("ERROR test_backup_boot: there is no valid backup image. Write it with update_backup_firmware_qspi first.");
		return;
	}
	u8 status = FlashReadStatus();
	if (status & FLASH_STATUS_BP_MASK) {
		SendLine("ERROR test_backup_boot: QSPI flash is write protected (status register 0x%02X)", status);
		return;
	}
	bool invalidated = FirmwareUpdate_invalidate_primary_image(/*use_network*/ true);
	TrialBoot_primary_image_changed();
	if (!invalidated) {
		SendLine("ERROR test_backup_boot: erasing the primary boot header failed");
		return;
	}
	SendLine("OK primary boot header erased; rebooting from the backup image. Afterwards write the primary image again.");
	FirmwareUpdate_reboot();
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
	/* The MULTIBOOT register survives a software reset, and the BootROM starts its search for a boot image there.
	 * After a fallback to the backup image it points to the backup slot, so reset it to start at the primary image
	 * (if that is invalid, the BootROM search continues to the backup image). */
	Xil_Out32(DEVCFG_UNLOCK, DEVCFG_UNLOCK_KEY);
	Xil_Out32(DEVCFG_MULTIBOOT, 0);
	/* a deliberate reboot during a trial boot is not a failure of the new firmware: FSBL starts a new trial */
	TrialBoot_before_reboot();
	Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY);
	Xil_Out32(SLCR_PSS_RST_CTRL, 1);  /* SOFT_RST: resets PS and PL, BootROM boots again */
	while (1);
}
