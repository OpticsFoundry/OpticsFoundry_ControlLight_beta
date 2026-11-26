#pragma once
/*
 * Firmware update over Ethernet (2026-11-26)
 *
 * Ethernet commands (dispatched in FireflyControlLoop):
 *
 *  get_firmware_info
 *      -> one line: build date/time, current boot mode, QSPI flash JEDEC ID
 *
 *  update_firmware_qspi
 *      <size in bytes>\n
 *      <CRC32 of image (zlib/IEEE 802.3), decimal>\n
 *      -> "Ready\n" (then send the raw BOOT.BIN bytes) or "ERROR ...\n"
 *      -> "OK received ...\n"   or "ERROR ...\n"   (after reception, CRC and boot header check)
 *      -> "OK programmed ...\n" or "ERROR ...\n"   (after erase, program and read-back verify)
 *      The image is written to QSPI flash offset 0, i.e. it replaces the current boot image.
 *
 *  reboot_system
 *      -> "OK rebooting\n", then a full system (PS + PL) software reset.
 *      The BootROM then boots from the medium selected by the boot mode jumpers.
 */

#include <stdbool.h>
#include "xil_types.h"

void FirmwareUpdate_send_firmware_info(void);
void FirmwareUpdate_update_qspi(u8* buffer, u32 buffer_size_in_bytes, bool sequence_running);
void FirmwareUpdate_reboot(void);
