/*
 * Trial boot of a new primary firmware image (2026-11-27), application part. See trial_boot.h.
 * The FSBL part is in OpticsFoundry_Seq_Zturn_Platform/zynq_fsbl/of_trial_boot.c; keep the constants in sync.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "xparameters.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "echo.h"
#include "firmware_update.h"
#include "trial_boot.h"

#define TRIAL_RECORD_SETTINGS_OFFSET 0x2000       /* QSPI 0xFF2000, see firmware_update.h */
#define TRIAL_RECORD_MAGIC           0x4254464F   /* "OFTB" */
#define TRIAL_STATE_PENDING          1
#define TRIAL_STATE_FAILED           2
#define TRIAL_STATE_CONFIRMED        3

#define TRIAL_TIMEOUT_MIN_S  30
#define TRIAL_TIMEOUT_MAX_S  1800

/* REBOOT_STATUS[31:24] survives all resets except power-on; FSBL uses bits 31:28 */
#define REBOOT_STATUS_REG               0xF8000258
#define REBOOT_STATUS_TRIAL_RUNNING     0x01000000
#define REBOOT_STATUS_TRIAL_FALLBACK    0x02000000
#define SLCR_UNLOCK_ADDR                0xF8000008
#define SLCR_UNLOCK_KEY                 0xDF0D
#define SLCR_BOOT_MODE                  0xF800025C
#define BOOT_MODE_MASK                  0x7
#define BOOT_MODE_QSPI                  0x1
#define DEVCFG_MULTIBOOT                0xF800702C
#define MULTIBOOT_OFFSET_MASK           0x1FFF
#define MULTIBOOT_UNIT                  0x8000

/* Cortex-A9 private watchdog (AWDT0), started by FSBL */
#define SCU_WDT_COUNTER     (XPAR_PS7_SCUWDT_0_BASEADDR + 0x04)
#define SCU_WDT_CONTROL     (XPAR_PS7_SCUWDT_0_BASEADDR + 0x08)
#define SCU_WDT_DISABLE     (XPAR_PS7_SCUWDT_0_BASEADDR + 0x14)
#define SCU_WDT_CONTROL_ENABLE  0x01
#define SCU_WDT_CONTROL_WD_MODE 0x08
#define SCU_WDT_PRESCALER       255
#define SCU_WDT_TICKS_PER_S     ((XPAR_PS7_CORTEXA9_0_CPU_CLK_FREQ_HZ / 2) / (SCU_WDT_PRESCALER + 1))

/* Zynq boot header: offset and length of the FSBL */
#define BOOT_HEADER_FSBL_OFFSET  0x30
#define BOOT_HEADER_FSBL_LENGTH  0x34

/* Marker strings: trial_reboot only accepts images whose FSBL and application contain them.
 * The FSBL marker is defined in of_trial_boot.c; it is searched for only within the FSBL partition of the image,
 * so the copy of the text below (in the application partition) doesn't count. */
const char TrialBootAppMarker[] = "OpticsFoundry app trial boot v1";
#define TRIAL_BOOT_FSBL_MARKER "OpticsFoundry FSBL trial boot v1"

typedef struct {
	u32 magic;
	u32 state;
	u32 image_size;
	u32 image_crc;     /* CRC32 of the image in the primary slot */
	u32 timeout_s;
	u32 record_crc;    /* CRC32 of the fields above */
} TrialRecord;

typedef enum { TRIAL_NONE, TRIAL_RUNNING, TRIAL_CONFIRMED, TRIAL_FAILED } TrialState;

static TrialState State = TRIAL_NONE;   /* what happened since this boot */
static TrialRecord Record;              /* copy of the record in the flash, if RecordValid */
static bool RecordValid = false;
static bool FailedPrimaryErased = false;

/* primary image written by update_firmware_qspi since this boot */
static bool NewPrimaryValid = false;
static u32 NewPrimarySize = 0;
static u32 NewPrimaryCrc = 0;
static bool NewPrimarySupportsTrial = false;
/* backup image written since this boot */
static bool NewBackupValid = false;
static u32 NewBackupCrc = 0;

/* ---------------- helpers ---------------- */

static u32 ReadLE32(const u8* p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool BootedFromQspi(void) {
	return (Xil_In32(SLCR_BOOT_MODE) & BOOT_MODE_MASK) == BOOT_MODE_QSPI;
}

static u32 BootedImageOffset(void) {
	return (Xil_In32(DEVCFG_MULTIBOOT) & MULTIBOOT_OFFSET_MASK) * MULTIBOOT_UNIT;
}

static void SetRebootStatusBits(u32 set, u32 clear) {
	Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY);
	Xil_Out32(REBOOT_STATUS_REG, (Xil_In32(REBOOT_STATUS_REG) & ~clear) | set);
}

static bool WatchdogActive(void) {
	const u32 mask = SCU_WDT_CONTROL_WD_MODE | SCU_WDT_CONTROL_ENABLE;
	return (Xil_In32(SCU_WDT_CONTROL) & mask) == mask;
}

static u32 WatchdogSecondsLeft(void) {
	return Xil_In32(SCU_WDT_COUNTER) / SCU_WDT_TICKS_PER_S;
}

static void StopWatchdog(void) {
	/* the disable sequence switches the watchdog back to timer mode, in which it can't reset the system */
	Xil_Out32(SCU_WDT_DISABLE, 0x12345678);
	Xil_Out32(SCU_WDT_DISABLE, 0x87654321);
	Xil_Out32(SCU_WDT_CONTROL, 0);
}

static bool ReadRecord(TrialRecord* record) {
	if (!FirmwareUpdate_read_settings(TRIAL_RECORD_SETTINGS_OFFSET, (u8*)record, sizeof(*record))) return false;
	return (record->magic == TRIAL_RECORD_MAGIC) && (FirmwareUpdate_crc32((const u8*)record, sizeof(*record) - 4) == record->record_crc);
}

static bool WriteRecord(u32 state, u32 image_size, u32 image_crc, u32 timeout_s) {
	TrialRecord record;
	record.magic = TRIAL_RECORD_MAGIC;
	record.state = state;
	record.image_size = image_size;
	record.image_crc = image_crc;
	record.timeout_s = timeout_s;
	record.record_crc = FirmwareUpdate_crc32((const u8*)&record, sizeof(record) - 4);
	if (!FirmwareUpdate_write_settings(TRIAL_RECORD_SETTINGS_OFFSET, (const u8*)&record, sizeof(record))) return false;
	Record = record;
	RecordValid = true;
	return true;
}

static bool EraseRecord(void) {
	if (!FirmwareUpdate_erase_settings(TRIAL_RECORD_SETTINGS_OFFSET)) return false;
	RecordValid = false;
	return true;
}

static bool RecordPending(void) {
	return RecordValid && (Record.state == TRIAL_STATE_PENDING);
}

static bool Contains(const u8* data, u32 length, const char* text) {
	const u32 n = (u32)strlen(text);
	if (length < n) return false;
	for (u32 i = 0; i + n <= length; i++) {
		if ((data[i] == (u8)text[0]) && (memcmp(data + i, text, n) == 0)) return true;
	}
	return false;
}

/* true if FSBL and application of the boot image contain the trial boot support */
static bool ImageSupportsTrialBoot(const u8* image, u32 size) {
	if (size < BOOT_HEADER_FSBL_LENGTH + 4) return false;
	u32 fsbl_offset = ReadLE32(image + BOOT_HEADER_FSBL_OFFSET);
	u32 fsbl_length = ReadLE32(image + BOOT_HEADER_FSBL_LENGTH);
	if ((fsbl_offset >= size) || (fsbl_length > size - fsbl_offset)) return false;
	if (!Contains(image + fsbl_offset, fsbl_length, TRIAL_BOOT_FSBL_MARKER)) return false;
	u32 rest = fsbl_offset + fsbl_length;
	return Contains(image + rest, size - rest, TrialBootAppMarker);
}

/* ---------------- start-up ---------------- */

void TrialBoot_init(void) {
	const u32 reboot_status = Xil_In32(REBOOT_STATUS_REG);
	const bool running_bit = (reboot_status & REBOOT_STATUS_TRIAL_RUNNING) != 0;
	const bool fallback_bit = (reboot_status & REBOOT_STATUS_TRIAL_FALLBACK) != 0;
	RecordValid = ReadRecord(&Record);
	xil_printf("Trial boot: REBOOT_STATUS 0x%08lX, trial record %s\r\n", (unsigned long)reboot_status,
			!RecordValid ? "none" : (Record.state == TRIAL_STATE_PENDING) ? "pending" : (Record.state == TRIAL_STATE_FAILED) ? "failed" :
			(Record.state == TRIAL_STATE_CONFIRMED) ? "confirmed" : "unknown");
	if (!BootedFromQspi()) return;   /* SD card or JTAG: nothing was booted from the flash; leave everything as it is */

	const u32 booted = BootedImageOffset();
	if (booted == QSPI_PRIMARY_IMAGE_OFFSET) {
		if (running_bit && RecordPending()) {
			State = TRIAL_RUNNING;
			SetRebootStatusBits(0, REBOOT_STATUS_TRIAL_FALLBACK);
			xil_printf("Trial boot: this firmware runs on trial (%s); unless the PC confirms it within %lu s, the watchdog resets the board and it boots the backup image\r\n",
					TrialBootAppMarker, (unsigned long)WatchdogSecondsLeft());
			return;
		}
		if (RecordPending()) {
			/* FSBL didn't start a trial: the primary image isn't the one the record was written for, there is no backup image,
			 * or the FSBL has no trial boot support */
			EraseRecord();
			xil_printf("Trial boot: no trial was started for the pending trial record; record removed\r\n");
		}
	} else if ((booted == QSPI_BACKUP_IMAGE_OFFSET) && RecordPending() && (running_bit || fallback_bit)) {
		/* the trial failed: FSBL fell back to the backup image (watchdog reset, or the new image couldn't be loaded) */
		State = TRIAL_FAILED;
		xil_printf("Trial boot: the new primary image (%lu bytes, CRC32 0x%08lX) was not confirmed in time. Running the backup image.\r\n",
				(unsigned long)Record.image_size, (unsigned long)Record.image_crc);
		/* erase the primary boot header first: if the board were switched off before, it would try the new image again (with trial) */
		FailedPrimaryErased = FirmwareUpdate_invalidate_primary_image(/*use_network*/ false);
		WriteRecord(TRIAL_STATE_FAILED, Record.image_size, Record.image_crc, Record.timeout_s);
		xil_printf("Trial boot: %s\r\n", FailedPrimaryErased ? "primary image erased; the board boots the backup image until a new primary image is written"
				: "erasing the primary image FAILED");
	} else if (RecordPending()) {
		EraseRecord();
		xil_printf("Trial boot: pending trial record without trial (primary image missing?); record removed\r\n");
	}
	SetRebootStatusBits(0, REBOOT_STATUS_TRIAL_RUNNING | REBOOT_STATUS_TRIAL_FALLBACK);
}

bool TrialBoot_running(void) {
	return State == TRIAL_RUNNING;
}

/* ---------------- notifications ---------------- */

/* the trial record describes an older primary image: remove it (not while a trial runs; updates are refused then anyway) */
static void ForgetOldTrial(void) {
	if (State == TRIAL_RUNNING) return;
	if (State == TRIAL_FAILED) State = TRIAL_NONE;
	if (RecordValid) EraseRecord();
}

void TrialBoot_primary_image_written(const u8* image, u32 size, u32 crc) {
	ForgetOldTrial();
	NewPrimaryValid = true;
	NewPrimarySize = size;
	NewPrimaryCrc = crc;
	NewPrimarySupportsTrial = ImageSupportsTrialBoot(image, size);
	xil_printf("Trial boot: new primary image %s trial boot support\r\n", NewPrimarySupportsTrial ? "has" : "has NO");
}

void TrialBoot_backup_image_written(u32 crc) {
	NewBackupValid = true;
	NewBackupCrc = crc;
}

void TrialBoot_primary_image_changed(void) {
	ForgetOldTrial();
	NewPrimaryValid = false;
}

void TrialBoot_before_reboot(void) {
	SetRebootStatusBits(0, REBOOT_STATUS_TRIAL_RUNNING | REBOOT_STATUS_TRIAL_FALLBACK);
}

/* ---------------- status ---------------- */

void TrialBoot_describe(char* text, size_t size) {
	if (size == 0) return;
	text[0] = 0;
	switch (State) {
	case TRIAL_RUNNING:
		snprintf(text, size, "; trial boot: RUNNING, not confirmed yet, watchdog resets to the backup image in %lu s (image %lu bytes, CRC32 0x%08lX)",
				(unsigned long)(WatchdogActive() ? WatchdogSecondsLeft() : 0), (unsigned long)Record.image_size, (unsigned long)Record.image_crc);
		break;
	case TRIAL_CONFIRMED:
		snprintf(text, size, "; trial boot: confirmed (image %lu bytes, CRC32 0x%08lX)", (unsigned long)Record.image_size, (unsigned long)Record.image_crc);
		break;
	case TRIAL_FAILED:
		snprintf(text, size, "; trial boot: FAILED, the new primary image (%lu bytes, CRC32 0x%08lX) was not confirmed in time; running the backup image%s",
				(unsigned long)Record.image_size, (unsigned long)Record.image_crc, FailedPrimaryErased ? ", primary image erased" : "");
		break;
	default:
		if (RecordValid && (Record.state == TRIAL_STATE_FAILED))
			snprintf(text, size, "; last trial boot FAILED (image %lu bytes, CRC32 0x%08lX)", (unsigned long)Record.image_size, (unsigned long)Record.image_crc);
		break;
	}
}

/* ---------------- Ethernet commands ---------------- */

void TrialBoot_reboot_command(bool sequence_running) {
	unsigned char* text = server_get_command(/*timeout*/ 10.0);
	if (text == NULL) {
		FirmwareUpdate_send_line("ERROR trial_reboot: timeout not received");
		return;
	}
	u32 timeout_s = strtoul((char*)text, NULL, 10);
	if (sequence_running) {
		FirmwareUpdate_send_line("ERROR trial_reboot: a sequence is running");
		return;
	}
	if (!BootedFromQspi()) {
		FirmwareUpdate_send_line("ERROR trial_reboot: the sequencer doesn't boot from its QSPI flash (boot mode jumpers); reboot with reboot_system");
		return;
	}
	if (!NewPrimaryValid) {
		FirmwareUpdate_send_line("ERROR trial_reboot: no new primary image was written since the sequencer booted; write it with update_firmware_qspi first");
		return;
	}
	if (!NewPrimarySupportsTrial) {
		FirmwareUpdate_send_line("ERROR trial_reboot: the new primary image has no trial boot support (firmware built before 2026-11-27); reboot with reboot_system instead");
		return;
	}
	if (!FirmwareUpdate_primary_image_valid()) {
		FirmwareUpdate_send_line("ERROR trial_reboot: the primary image in the flash is invalid");
		return;
	}
	if (!FirmwareUpdate_backup_image_valid()) {
		FirmwareUpdate_send_line("ERROR trial_reboot: there is no valid backup image to fall back to; reboot with reboot_system, or write a backup image first");
		return;
	}
	if (NewBackupValid && (NewBackupCrc == NewPrimaryCrc)) {
		FirmwareUpdate_send_line("ERROR trial_reboot: the backup image is the same as the new primary image, so a failed trial couldn't fall back to a working firmware. "
				"Write the backup image only after the new firmware has been confirmed.");
		return;
	}
	if (timeout_s < TRIAL_TIMEOUT_MIN_S) timeout_s = TRIAL_TIMEOUT_MIN_S;
	if (timeout_s > TRIAL_TIMEOUT_MAX_S) timeout_s = TRIAL_TIMEOUT_MAX_S;
	if (!WriteRecord(TRIAL_STATE_PENDING, NewPrimarySize, NewPrimaryCrc, timeout_s)) {
		FirmwareUpdate_send_line("ERROR trial_reboot: writing the trial record into the QSPI flash failed");
		return;
	}
	FirmwareUpdate_send_line("OK trial boot armed: new primary image %lu bytes, CRC32 0x%08lX; unless it is confirmed within %lu s after the reboot, the sequencer boots its backup image",
			(unsigned long)NewPrimarySize, (unsigned long)NewPrimaryCrc, (unsigned long)timeout_s);
	FirmwareUpdate_reboot();
}

void TrialBoot_confirm_command(void) {
	if (State != TRIAL_RUNNING) {
		FirmwareUpdate_send_line("OK no trial boot running");
		return;
	}
	StopWatchdog();
	SetRebootStatusBits(0, REBOOT_STATUS_TRIAL_RUNNING | REBOOT_STATUS_TRIAL_FALLBACK);
	State = TRIAL_CONFIRMED;
	if (WatchdogActive()) {
		FirmwareUpdate_send_line("ERROR confirm_firmware: the trial boot watchdog could not be stopped");
		return;
	}
	/* if the record can't be marked confirmed, at least make sure it isn't pending any more, otherwise the next power-on starts a new trial */
	bool recorded = WriteRecord(TRIAL_STATE_CONFIRMED, Record.image_size, Record.image_crc, Record.timeout_s) || EraseRecord();
	if (!recorded) {
		FirmwareUpdate_send_line("ERROR confirm_firmware: watchdog stopped, but the trial record in the QSPI flash couldn't be updated; the trial will run again after the next power-on");
		return;
	}
	FirmwareUpdate_send_line("OK firmware confirmed; trial boot watchdog stopped");
}
