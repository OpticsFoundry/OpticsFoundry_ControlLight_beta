#pragma once
/*
 * Sequencer identity and network discovery (2026-11-26)
 *
 * MAC address: 02:4F:46:xx:xx:xx ("OF", locally administered, unicast). xx:xx:xx are the lower
 * 24 bits of the CRC32 (zlib) of the 64 bit factory unique ID of the Winbond QSPI flash, so each
 * board has its own MAC address without firmware changes. If the flash has no supported unique ID,
 * the previous fixed MAC address 00:0A:35:F1:01:03 is used.
 *
 * Discovery: send the UDP datagram "OpticsFoundry_discover" to port 57979, e.g. as broadcast.
 * Each sequencer answers with one line of text to the sender's address and port:
 *   "OpticsFoundry sequencer; MAC ...; IP ...; TCP port 57978; build ...; ..."
 * The answer is sent by unicast if the sender is in the sequencer's subnet, otherwise by broadcast,
 * so that sequencers with an IP address of another subnet can be found, too.
 */

#include <stddef.h>
#include <stdbool.h>
#include "xil_types.h"

#define SEQUENCER_TCP_PORT            57978
#define SEQUENCER_DISCOVERY_UDP_PORT  57979
#define SEQUENCER_DISCOVERY_REQUEST   "OpticsFoundry_discover"

void SequencerIdentity_get_mac(unsigned char mac[6]);       /* call before the network interface is set up */
bool SequencerIdentity_mac_from_unique_id(void);
void SequencerIdentity_start_discovery(void);                /* call once the network interface is up */
void SequencerIdentity_describe(char* text, size_t size);   /* "MAC ...; IP ..." */
