#pragma once
/*
 * Installing the firmware from an SD card into the QSPI flash (2026-11-26)
 *
 * Installer SD card: a FAT16/FAT32 formatted card with, in its root folder,
 *   BOOT.BIN   the firmware boot image
 *   INSTALL    an (empty) marker file; any extension, e.g. INSTALL.TXT
 *
 * When the board boots from such a card (boot mode jumpers on SD card), the firmware copies BOOT.BIN into
 * both QSPI image slots (primary, then backup) before it starts the network, and verifies them. Slots that
 * already contain the image are not written again. Without the INSTALL file, booting from SD card works
 * as usual and the QSPI flash isn't touched.
 * Signals: RGB LED blue while installing, then green and two short beeps (installed) or one short beep
 * (QSPI flash already up to date); red and three long beeps on failure. Details on the USB UART (115200 baud)
 * and in the firmware info (get_firmware_info).
 *
 * Ethernet command install_from_sd: does the same with the SD card in the slot of a running board,
 * independent of the boot mode and without the INSTALL file. Answers lines starting with "OK" or "ERROR";
 * the last line starts with "OK installed", "OK QSPI flash already" or "ERROR".
 */

#include <stdbool.h>
#include <stddef.h>
#include "xil_types.h"

void SdInstaller_run_at_boot(void);                  /* call at start-up, before the network is set up */
void SdInstaller_install_command(bool sequence_running);
void SdInstaller_describe(char* text, size_t size);  /* "; SD card install at boot: ..." or "" */
