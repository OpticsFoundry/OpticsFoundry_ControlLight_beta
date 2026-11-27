#pragma once
/*
 * Trial boot of a new primary firmware image (2026-11-27)
 *
 * The backup image (see firmware_update.h) helps if the primary image is missing or can't be loaded. It doesn't help if a
 * new primary image boots but doesn't work, e.g. hangs or has no network connection. Trial boot covers that case:
 *
 *  1. The PC writes the new primary image (update_firmware_qspi) and sends trial_reboot. The running firmware stores a
 *     "pending" trial record (size and CRC32 of the new image, timeout) in the QSPI flash at 0xFF2000 and reboots.
 *  2. The FSBL of the new image (zynq_fsbl/of_trial_boot.c) finds the pending record, checks that the primary image is the
 *     one the record was written for and that there is a valid backup image, sets the TRIAL_RUNNING bit in the
 *     REBOOT_STATUS register and starts the CPU private watchdog (AWDT0, resets the whole system) before loading the image.
 *  3. The PC reconnects, checks with get_firmware_info that the new image runs on trial, and sends confirm_firmware.
 *     The firmware stops the watchdog, clears the TRIAL_RUNNING bit and marks the record "confirmed". From then on the
 *     watchdog is off, as without trial boot.
 *  4. Without confirmation in time, the watchdog resets the system. REBOOT_STATUS[31:24] survives that reset, so the FSBL
 *     sees TRIAL_RUNNING still set, sets TRIAL_FALLBACK and boots the backup image (MULTIBOOT register + software reset).
 *     The backup firmware (if it has trial boot support) marks the record "failed" and erases the boot header of the
 *     primary image, so that the board keeps booting the backup image, also after a power cycle.
 *     get_firmware_info then reports the failed trial.
 *
 * A deliberate reboot during the trial (reboot_system, network settings) clears TRIAL_RUNNING, so the FSBL starts a new
 * trial instead of falling back. A power cycle during the trial also starts a new trial (REBOOT_STATUS is cleared at power-on,
 * the record stays pending).
 *
 * trial_reboot only accepts images that contain trial boot support in both FSBL and application (marker strings), so that
 * a downgrade to older firmware doesn't leave a pending record behind. Watchdog timeout: 30 .. 1800 s.
 *
 * Ethernet commands (dispatched in FireflyControlLoop):
 *
 *  trial_reboot
 *      <timeout in seconds>\n
 *      -> "OK trial boot armed ...\n", then "OK rebooting\n" and a reboot
 *      -> or "ERROR trial_reboot: ...\n" (no reboot), e.g. if no primary image was written since the last boot, the new image
 *         has no trial boot support, or there is no valid backup image
 *
 *  confirm_firmware
 *      -> "OK firmware confirmed; trial boot watchdog stopped\n"  or  "OK no trial boot running\n"
 *      -> or "ERROR confirm_firmware: ...\n" (watchdog stopped anyway)
 *
 * get_firmware_info contains "; trial boot: RUNNING, ... image CRC32 0x........" while a trial runs, or "; trial boot: FAILED ..."
 * after a fallback.
 */

#include <stdbool.h>
#include <stddef.h>
#include "xil_types.h"

void TrialBoot_init(void);   /* at start-up, after FirmwareUpdate_init and before the network is set up */

bool TrialBoot_running(void);   /* a trial boot runs and hasn't been confirmed yet */

/* notifications from firmware_update.c */
void TrialBoot_primary_image_written(const u8* image, u32 size, u32 crc);  /* by update_firmware_qspi */
void TrialBoot_backup_image_written(u32 crc);
void TrialBoot_primary_image_changed(void);  /* otherwise changed (SD card install, failed write, test_backup_boot) */
void TrialBoot_before_reboot(void);          /* clears the trial bits in REBOOT_STATUS before a deliberate software reset */

/* "; trial boot: ..." or "" */
void TrialBoot_describe(char* text, size_t size);

/* Ethernet commands */
void TrialBoot_reboot_command(bool sequence_running);
void TrialBoot_confirm_command(void);
