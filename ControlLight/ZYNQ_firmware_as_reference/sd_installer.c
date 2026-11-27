/*
 * Installing the firmware from an SD card into the QSPI flash (2026-11-26), see sd_installer.h.
 * Contains a minimal read-only FAT16/FAT32 reader for files in the root folder (8.3 names).
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "xparameters.h"
#include "xsdps.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "lwip/tcp.h"
#include "echo.h"
#include "FireflyControl.h"
#include "firmware_update.h"
#include "sd_installer.h"

extern struct tcp_pcb *last_tpcb;

#define SLCR_BOOT_MODE  0xF800025C
#define BOOT_MODE_MASK  0x7
#define BOOT_MODE_SD    0x5
#define SECTOR_SIZE     512
#define MAX_IMAGE_SIZE  0x800000

static XSdPs Sd;
static bool SdConfigured = false;
static u8 Sector[SECTOR_SIZE] __attribute__((aligned(64)));
static char BootInstallStatus[220] = "";

typedef struct {
	bool fat32;
	u32 sectors_per_cluster;
	u32 fat_lba;           /* first sector of the first FAT */
	u32 root_dir_lba;      /* FAT16: fixed root directory */
	u32 root_dir_sectors;  /* FAT16 */
	u32 root_cluster;      /* FAT32 */
	u32 data_lba;          /* sector of cluster 2 */
	u32 cluster_count;
} Fat;

typedef struct {
	bool found;
	u32 first_cluster;
	u32 size;
} FileEntry;

static void Report(bool use_network, const char* format, ...) {
	char line[300];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line) - 2, format, args);
	va_end(args);
	xil_printf("%s\r\n", line);
	if (!use_network) return;
	strcat(line, "\n");
	server_send_ascii(line);
	if (last_tpcb) tcp_output(last_tpcb);
}

static u16 Le16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 Le32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static bool ReadSectors(u32 lba, u32 count, u8* buffer) {
	return XSdPs_ReadPolled(&Sd, lba, count, buffer) == XST_SUCCESS;
}

static bool SdInit(char* error, size_t error_size) {
	if (!SdConfigured) {
		XSdPs_Config* config = XSdPs_LookupConfig(XPAR_XSDPS_0_DEVICE_ID);
		if (config == NULL) {
			snprintf(error, error_size, "no SD card controller");
			return false;
		}
		s32 status = XSdPs_CfgInitialize(&Sd, config, config->BaseAddress);
		if ((status != XST_SUCCESS) && (status != XST_DEVICE_IS_STARTED)) {
			snprintf(error, error_size, "SD card controller initialization failed");
			return false;
		}
		SdConfigured = true;
	}
	if (XSdPs_CardInitialize(&Sd) != XST_SUCCESS) {
		snprintf(error, error_size, "no SD card, or SD card initialization failed");
		return false;
	}
	return true;
}

static bool MountFat(Fat* fat, char* error, size_t error_size) {
	if (!ReadSectors(0, 1, Sector)) {
		snprintf(error, error_size, "can't read SD card sector 0");
		return false;
	}
	if ((Sector[510] != 0x55) || (Sector[511] != 0xAA)) {
		snprintf(error, error_size, "SD card has neither a partition table nor a FAT boot sector");
		return false;
	}
	u32 volume_lba = 0;
	bool boot_sector = ((Sector[0] == 0xEB) || (Sector[0] == 0xE9)) && (Le16(Sector + 11) == SECTOR_SIZE);
	if (!boot_sector) {  /* partition table: use the first FAT partition */
		bool found = false;
		for (int i = 0; (i < 4) && !found; i++) {
			const u8* entry = Sector + 446 + 16 * i;
			u8 type = entry[4];
			if ((type == 0x04) || (type == 0x06) || (type == 0x0E) || (type == 0x0B) || (type == 0x0C)) {
				volume_lba = Le32(entry + 8);
				found = true;
			}
		}
		if (!found) {
			snprintf(error, error_size, "SD card has no FAT16/FAT32 partition");
			return false;
		}
		if (!ReadSectors(volume_lba, 1, Sector)) {
			snprintf(error, error_size, "can't read the FAT boot sector");
			return false;
		}
	}
	u32 bytes_per_sector = Le16(Sector + 11);
	u32 sectors_per_cluster = Sector[13];
	u32 reserved_sectors = Le16(Sector + 14);
	u32 number_of_fats = Sector[16];
	u32 root_entries = Le16(Sector + 17);
	u32 total_sectors = Le16(Sector + 19) ? Le16(Sector + 19) : Le32(Sector + 32);
	u32 fat_size = Le16(Sector + 22) ? Le16(Sector + 22) : Le32(Sector + 36);
	if ((bytes_per_sector != SECTOR_SIZE) || (sectors_per_cluster == 0) || (number_of_fats == 0) || (fat_size == 0)) {
		snprintf(error, error_size, "unsupported FAT file system (sector size %lu)", (unsigned long)bytes_per_sector);
		return false;
	}
	fat->sectors_per_cluster = sectors_per_cluster;
	fat->fat_lba = volume_lba + reserved_sectors;
	fat->root_dir_lba = fat->fat_lba + number_of_fats * fat_size;
	fat->root_dir_sectors = (root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	fat->data_lba = fat->root_dir_lba + fat->root_dir_sectors;
	u32 used = reserved_sectors + number_of_fats * fat_size + fat->root_dir_sectors;
	fat->cluster_count = (total_sectors > used) ? (total_sectors - used) / sectors_per_cluster : 0;
	if (fat->cluster_count < 4085) {
		snprintf(error, error_size, "FAT12 is not supported; format the SD card with FAT32");
		return false;
	}
	fat->fat32 = (fat->cluster_count >= 65525);
	fat->root_cluster = fat->fat32 ? Le32(Sector + 44) : 0;
	return true;
}

static u32 ClusterLba(const Fat* fat, u32 cluster) {
	return fat->data_lba + (cluster - 2) * fat->sectors_per_cluster;
}

static bool IsValidCluster(const Fat* fat, u32 cluster) {
	return (cluster >= 2) && (cluster < fat->cluster_count + 2);
}

/* next cluster of a chain; *end is set at the end of the chain */
static bool NextCluster(const Fat* fat, u32 cluster, u32* next, bool* end) {
	u32 offset = cluster * (fat->fat32 ? 4 : 2);
	if (!ReadSectors(fat->fat_lba + offset / SECTOR_SIZE, 1, Sector)) return false;
	u32 value = fat->fat32 ? (Le32(Sector + offset % SECTOR_SIZE) & 0x0FFFFFFF) : Le16(Sector + offset % SECTOR_SIZE);
	*end = fat->fat32 ? (value >= 0x0FFFFFF8) : (value >= 0xFFF8);
	*next = value;
	return *end || IsValidCluster(fat, value);
}

/* looks at the 32-byte directory entries in Sector; returns false at the end of the directory */
static bool ScanDirectorySector(FileEntry* boot_bin, FileEntry* install) {
	for (int i = 0; i < SECTOR_SIZE; i += 32) {
		const u8* entry = Sector + i;
		if (entry[0] == 0x00) return false;   /* end of directory */
		if (entry[0] == 0xE5) continue;       /* deleted */
		u8 attributes = entry[11];
		if ((attributes & 0x0F) == 0x0F) continue;  /* long file name part */
		if (attributes & 0x18) continue;            /* volume label or directory */
		FileEntry* target = NULL;
		if (memcmp(entry, "BOOT    BIN", 11) == 0) target = boot_bin;
		else if (memcmp(entry, "INSTALL ", 8) == 0) target = install;
		if (target != NULL) {
			target->found = true;
			target->first_cluster = ((u32)Le16(entry + 20) << 16) | Le16(entry + 26);
			target->size = Le32(entry + 28);
		}
	}
	return true;
}

static bool FindRootFiles(const Fat* fat, FileEntry* boot_bin, FileEntry* install, char* error, size_t error_size) {
	memset(boot_bin, 0, sizeof(*boot_bin));
	memset(install, 0, sizeof(*install));
	if (!fat->fat32) {
		for (u32 s = 0; s < fat->root_dir_sectors; s++) {
			if (!ReadSectors(fat->root_dir_lba + s, 1, Sector)) {
				snprintf(error, error_size, "can't read the root folder");
				return false;
			}
			if (!ScanDirectorySector(boot_bin, install)) return true;
		}
		return true;
	}
	u32 cluster = fat->root_cluster;
	for (u32 clusters = 0; clusters < 65536; clusters++) {
		if (!IsValidCluster(fat, cluster)) break;
		for (u32 s = 0; s < fat->sectors_per_cluster; s++) {
			if (!ReadSectors(ClusterLba(fat, cluster) + s, 1, Sector)) {
				snprintf(error, error_size, "can't read the root folder");
				return false;
			}
			if (!ScanDirectorySector(boot_bin, install)) return true;
		}
		bool end;
		if (!NextCluster(fat, cluster, &cluster, &end)) {
			snprintf(error, error_size, "root folder cluster chain broken");
			return false;
		}
		if (end) break;
	}
	return true;
}

static bool ReadFile(const Fat* fat, const FileEntry* file, u8* buffer, u32 buffer_size, char* error, size_t error_size) {
	u32 cluster_bytes = fat->sectors_per_cluster * SECTOR_SIZE;
	if (((file->size + cluster_bytes - 1) / cluster_bytes) * cluster_bytes > buffer_size) {
		snprintf(error, error_size, "BOOT.BIN too large (%lu bytes)", (unsigned long)file->size);
		return false;
	}
	u32 cluster = file->first_cluster;
	for (u32 done = 0; done < file->size; done += cluster_bytes) {
		if (!IsValidCluster(fat, cluster)) {
			snprintf(error, error_size, "BOOT.BIN cluster chain broken");
			return false;
		}
		if (!ReadSectors(ClusterLba(fat, cluster), fat->sectors_per_cluster, buffer + done)) {
			snprintf(error, error_size, "can't read BOOT.BIN from the SD card");
			return false;
		}
		if (done + cluster_bytes < file->size) {
			bool end;
			if (!NextCluster(fat, cluster, &cluster, &end) || end) {
				snprintf(error, error_size, "BOOT.BIN cluster chain broken");
				return false;
			}
		}
	}
	return true;
}

/* reads BOOT.BIN into buffer; *install_requested tells whether the INSTALL marker file exists.
 * only_if_install_requested: don't read BOOT.BIN if there is no INSTALL file */
static bool ReadBootBin(bool only_if_install_requested, u8* buffer, u32 buffer_size, u32* size, bool* install_requested, bool* fat32, char* error, size_t error_size) {
	Fat fat;
	FileEntry boot_bin, install;
	*install_requested = false;
	if (!SdInit(error, error_size)) return false;
	if (!MountFat(&fat, error, error_size)) return false;
	*fat32 = fat.fat32;
	if (!FindRootFiles(&fat, &boot_bin, &install, error, error_size)) return false;
	*install_requested = install.found;
	if (only_if_install_requested && !install.found) {
		snprintf(error, error_size, "no INSTALL file on the SD card");
		return false;
	}
	if (!boot_bin.found) {
		snprintf(error, error_size, "no BOOT.BIN in the root folder of the SD card");
		return false;
	}
	if ((boot_bin.size == 0) || (boot_bin.size > MAX_IMAGE_SIZE)) {
		snprintf(error, error_size, "BOOT.BIN has an invalid size (%lu bytes)", (unsigned long)boot_bin.size);
		return false;
	}
	if (!ReadFile(&fat, &boot_bin, buffer, buffer_size, error, error_size)) return false;
	*size = boot_bin.size;
	return true;
}

static void WaitMs(u32 ms) {
	XTime start, now;
	XTime_GetTime(&start);
	do { XTime_GetTime(&now); } while ((now - start) < (XTime)ms * (COUNTS_PER_SECOND / 1000));
}

static void Beep(int count, u32 on_ms, u32 off_ms) {
	for (int i = 0; i < count; i++) {
		SwitchBuzzer(1);
		WaitMs(on_ms);
		SwitchBuzzer(0);
		WaitMs(off_ms);
	}
}

/* blue status LED toggles with each flash block written or checked, but at most every 100 ms, so that the user sees that the installation is progressing */
#define BLINK_MIN_INTERVAL_MS 100
static bool BlinkLedOn = true;
static XTime BlinkLastToggle = 0;

static void BlinkProgress(void) {
	XTime now;
	XTime_GetTime(&now);
	if ((now - BlinkLastToggle) < (XTime)BLINK_MIN_INTERVAL_MS * (COUNTS_PER_SECOND / 1000)) return;
	BlinkLastToggle = now;
	BlinkLedOn = !BlinkLedOn;
	SetLED0Color(0, 0, BlinkLedOn ? 1 : 0);
}

void SdInstaller_run_at_boot(void) {
	if ((Xil_In32(SLCR_BOOT_MODE) & BOOT_MODE_MASK) != BOOT_MODE_SD) return;
	u32 buffer_size;
	u8* buffer = FireflyControl_get_scratch_buffer(&buffer_size);
	char error[160];
	u32 size = 0;
	bool install_requested = false, fat32 = false;
	bool read_ok = ReadBootBin(/*only_if_install_requested*/ true, buffer, buffer_size, &size, &install_requested, &fat32, error, sizeof(error));
	if (!install_requested) {
		xil_printf("SD card installer: not active (%s)\r\n", error);
		return;
	}
	xil_printf("\r\n==== SD card installer: writing BOOT.BIN into the QSPI flash. Don't switch the board off. ====\r\n");
	FireflyControl_init_hardware();
	SetLED0Color(0, 0, 1);
	char summary[220];
	int result = FIRMWARE_INSTALL_FAILED;
	if (read_ok) {
		xil_printf("SD card installer: BOOT.BIN %lu bytes (%s)\r\n", (unsigned long)size, fat32 ? "FAT32" : "FAT16");
		XTime_GetTime(&BlinkLastToggle);
		FirmwareUpdate_set_progress_callback(BlinkProgress);
		result = FirmwareUpdate_install_image(buffer, size, /*use_network*/ false, summary, sizeof(summary));
		FirmwareUpdate_set_progress_callback(NULL);
	} else {
		snprintf(summary, sizeof(summary), "failed: %s", error);
	}
	snprintf(BootInstallStatus, sizeof(BootInstallStatus), "%s", summary);
	if (result == FIRMWARE_INSTALL_FAILED) {
		SetLED0Color(1, 0, 0);
		Beep(3, 700, 300);
	} else {
		SetLED0Color(0, 1, 0);
		Beep((result == FIRMWARE_INSTALL_WRITTEN) ? 2 : 1, 150, 150);
	}
	xil_printf("==== SD card installer: %s ====\r\n", summary);
	if (result != FIRMWARE_INSTALL_FAILED)
		xil_printf("To boot from the QSPI flash: switch the board off, set both boot mode jumpers to closed, remove the SD card, switch the board on.\r\n\r\n");
}

void SdInstaller_install_command(bool sequence_running) {
	if (sequence_running) {
		Report(true, "ERROR install_from_sd: a sequence is running");
		return;
	}
	u32 buffer_size;
	u8* buffer = FireflyControl_get_scratch_buffer(&buffer_size);
	char error[160];
	u32 size = 0;
	bool install_requested = false, fat32 = false;
	if (!ReadBootBin(/*only_if_install_requested*/ false, buffer, buffer_size, &size, &install_requested, &fat32, error, sizeof(error))) {
		Report(true, "ERROR install_from_sd: %s", error);
		return;
	}
	Report(true, "OK SD card: BOOT.BIN %lu bytes (%s); writing it into the QSPI flash", (unsigned long)size, fat32 ? "FAT32" : "FAT16");
	char summary[220];
	int result = FirmwareUpdate_install_image(buffer, size, /*use_network*/ true, summary, sizeof(summary));
	if (result == FIRMWARE_INSTALL_FAILED) Report(true, "ERROR install_from_sd: %s", summary);
	else Report(true, "OK %s", summary);
}

void SdInstaller_describe(char* text, size_t size) {
	if (BootInstallStatus[0]) snprintf(text, size, "; SD card install at boot: %s", BootInstallStatus);
	else if (size > 0) text[0] = 0;
}
