#pragma once
/*
 * Firmware update over Ethernet, backup boot image (2026-11-26)
 *
 * QSPI flash layout (16 MB):
 *   0x000000  primary boot image, written by update_firmware_qspi      (max 8 MB)
 *   0x800000  backup boot image, written by update_backup_firmware_qspi (max 8 MB - 64 KB)
 *   0xFF0000  settings sector (reserved)
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
 *  reboot_system
 *      -> "OK rebooting\n", then a full system (PS + PL) software reset.
 *      The BootROM then boots from the medium selected by the boot mode jumpers.
 */

#include <stdbool.h>
#include <stddef.h>
#include "xil_types.h"

#define QSPI_PRIMARY_IMAGE_OFFSET  0x000000
#define QSPI_BACKUP_IMAGE_OFFSET   0x800000
#define QSPI_SETTINGS_OFFSET       0xFF0000

void FirmwareUpdate_send_firmware_info(void);
void FirmwareUpdate_update_qspi(u8* buffer, u32 buffer_size_in_bytes, bool sequence_running, bool backup_image);
void FirmwareUpdate_test_backup_boot(bool sequence_running);
void FirmwareUpdate_reboot(void);

/* Reads and caches flash IDs and image slot status. Called automatically; can be called before the network is set up. */
void FirmwareUpdate_init(void);

/* QSPI flash identification */
bool FirmwareUpdate_read_flash_jedec_id(u8 id[3]);
bool FirmwareUpdate_read_flash_unique_id(u8 unique_id[8]);  /* false if the flash has no supported unique ID */

/* "build ...; boot mode ...; image ...; primary image ...; backup image ...; QSPI flash ID ...; flash unique ID ..."; doesn't access the flash */
void FirmwareUpdate_describe(char* text, size_t size);

u32 FirmwareUpdate_crc32(const u8* data, u32 length);  /* zlib / IEEE 802.3 CRC32 */
