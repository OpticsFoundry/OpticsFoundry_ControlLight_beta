#pragma once

#include <string>
#include <vector>

// Finds OpticsFoundry sequencers on all local IPv4 networks (sequencer firmware from 2026-11-26 on).
// Sends the UDP datagram "OpticsFoundry_discover" to port 57979 as broadcast on every network interface and
// collects the answers, one line per sequencer, e.g.
// "OpticsFoundry sequencer; MAC 02:4F:46:E2:9D:95; IP 192.168.1.159; TCP port 57978; build ...; boot mode QSPI; ..."
// Duplicate answers (a sequencer reached over several interfaces) are removed.
// Returns false only if the discovery couldn't be run at all (error describes why); finding no sequencer is not an error.
bool DiscoverSequencers(std::vector<std::string>& sequencers, double timeout_in_seconds, std::string& error);
