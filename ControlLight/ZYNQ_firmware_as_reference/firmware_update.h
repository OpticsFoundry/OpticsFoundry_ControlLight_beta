#pragma once
/*
 * Firmware update over Ethernet, backup boot image (2026-11-26)
 *
 * QSPI flash layout (16 MB):
 *   0x000000  primary boot image, written by update_firmware_qspi      (max 8 MB)
 *   0x800000  backup boot image, written by update_backup_firmware_qspi (max 8 MB - 64 KB)
 *   0xFF0000  settings sector (64 KB): two copies of the settings record in the 4 KB subsectors at
 *             0xFF0000 and 0xFF1000, see network_settings.h; trial boot record at 0xFF2000, see trial_boot.h
 *
 * The Zynq BootROM boots the first valid boot image it finds, searching the flash in
 * 32 KB steps from offset 0. Images are written with their boot header (first flash page)
 * last, so an interrupted update leaves no valid header at the start of the slot, and the
 * BootROM falls back to the backup image. FSBL also falls back to the next image if it
 * can't load the partitions of an image.
 *
 * Ethernet commands (dispatched in FireflyControlLoop):
 *
 *  get_firmware_info
 *      -> one line: build date/time, boot mode, image booted from, backup image status,
 *         MAC address, QSPI flash JEDEC ID and unique ID
 *
 *  update_firmware_qspi            (writes the primary image)
 *  update_backup_firmware_qspi     (writes the backup image)
 *      <size in bytes>\n
 *      <CRC32 of image (zlib/IEEE 802.3), decimal>\n
 *      -> "Ready\n" (then send the raw BOOT.BIN bytes) or "ERROR ...\n"
 *      -> "OK received ...\n"   or "ERROR ...\n"   (after reception, CRC and boot header check)
 *      -> "OK programmed ...\n" or "ERROR ...\n"   (after erase, program and read-back verify)
 *
 *  test_backup_boot
 *      -> "OK ...\n" then erases the boot header of the primary image and reboots, so that the
 *         board boots the backup image. Refused if there is no valid backup image.
 *         Afterwards, write the primary image again with update_firmware_qspi.
 *      -> or "ERROR ...\n"
 *
 *  install_from_sd   (see sd_installer.h)
 *      writes BOOT.BIN from the SD card into both slots
 *
 *  read_firmware_image
 *      -> "OK <primary|backup> <size in bytes> <CRC32, decimal>\n", then the raw boot image (a BOOT.BIN)
 *      -> or "ERROR ...\n"
 *      Sends the image the board booted from QSPI flash (after booting from SD card: the primary image,
 *      or the backup image if the primary one is invalid). Its length is taken from the partition headers.
 *
 *  reboot_system
 *      -> "OK rebooting\n", then a full system (PS + PL) software reset.
 *      The BootROM then boots from the medium selected by the boot mode jumpers.
 *
 *  trial_reboot, confirm_firmware   (see trial_boot.h)
 *      reboot into a new primary image on trial, and confirm it afterwards
 *
 *  While a new firmware runs on trial and hasn't been confirmed, update_firmware_qspi, update_backup_firmware_qspi and
 *  install_from_sd are refused.
 */

#include <stdbool.h>
#include <stddef.h>
#include "xil_types.h"

#define QSPI_PRIMARY_IMAGE_OFFSET  0x000000
#define QSPI_BACKUP_IMAGE_OFFSET   0x800000
#define QSPI_SETTINGS_OFFSET       0xFF0000
#define QSPI_SETTINGS_SIZE         0x10000

void FirmwareUpdate_send_firmware_info(void);
void FirmwareUpdate_update_qspi(u8* buffer, u32 buffer_size_in_bytes, bool sequence_running, bool backup_image);
void FirmwareUpdate_test_backup_boot(bool sequence_running);
void FirmwareUpdate_send_image(u8* buffer, u32 buffer_size, bool sequence_running);
void FirmwareUpdate_reboot(void);

/* Reads and caches flash IDs and image slot status. Called automatically; can be called before the network is set up. */
void FirmwareUpdate_init(void);

/* QSPI flash identification */
bool FirmwareUpdate_read_flash_jedec_id(u8 id[3]);
bool FirmwareUpdate_read_flash_unique_id(u8 unique_id[8]);  /* false if the flash has no supported unique ID */

/* "build ...; boot mode ...; image ...; primary image ...; backup image ...; QSPI flash ID ...; flash unique ID ..."; doesn't access the flash */
void FirmwareUpdate_describe(char* text, size_t size);

u32 FirmwareUpdate_crc32(const u8* data, u32 length);  /* zlib / IEEE 802.3 CRC32 */

/* Settings sector access; offset is relative to QSPI_SETTINGS_OFFSET. Both return false if the flash is busy with another
 * operation, not available or write protected. Reading: length <= 4096 bytes.
 * Writing: erases the 4 KB subsector starting at offset (must be 4 KB aligned), programs length <= 256 bytes and verifies them.
 * Doesn't service the network (takes typically 50 ms, at most about 1 s). */
bool FirmwareUpdate_read_settings(u32 offset, u8* data, u32 length);
bool FirmwareUpdate_write_settings(u32 offset, const u8* data, u32 length);
bool FirmwareUpdate_erase_settings(u32 offset);  /* erases the 4 KB subsector starting at offset (4 KB aligned) */

/* image slot status, cached (doesn't access the flash) */
bool FirmwareUpdate_primary_image_valid(void);
bool FirmwareUpdate_backup_image_valid(void);

/* Erases the first sector of the primary image slot (with its boot header), so that the BootROM boots the backup image.
 * use_network: service the network while erasing (false at start-up). Returns true if the primary image is invalid afterwards. */
bool FirmwareUpdate_invalidate_primary_image(bool use_network);

/* sends one line of text to the connected PC (and the UART), like the firmware update commands do */
void FirmwareUpdate_send_line(const char* format, ...);

/* Writes a boot image into both QSPI slots (primary first), skipping slots that already contain it.
 * use_network: report progress to the connected PC and service the network while writing (false at start-up).
 * Returns one of the FIRMWARE_INSTALL_ values; summary receives a one-line description. */
#define FIRMWARE_INSTALL_FAILED      0
#define FIRMWARE_INSTALL_WRITTEN     1
#define FIRMWARE_INSTALL_UP_TO_DATE  2
int FirmwareUpdate_install_image(const u8* image, u32 size, bool use_network, char* summary, size_t summary_size);

/* Optional callback, called once per 64 KB flash block while erasing, programming, verifying or comparing a boot image
 * (every 20 to 220 ms), e.g. to blink an LED. NULL: none. */
void FirmwareUpdate_set_progress_callback(void (*callback)(void));
