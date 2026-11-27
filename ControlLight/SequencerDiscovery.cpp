// UDP discovery of OpticsFoundry sequencers, see SequencerDiscovery.h
// This file intentionally doesn't include the MFC based headers of this project, so that winsock2.h can be used.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif
typedef SOCKET SocketHandle;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#define CloseSocketHandle closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
typedef int SocketHandle;
#define INVALID_SOCKET_HANDLE (-1)
#define CloseSocketHandle close
#endif

#include "SequencerDiscovery.h"
#include <chrono>
#include <cstring>
#include <set>
#include <cctype>

static const unsigned short DiscoveryPort = 57979;
static const char* DiscoveryRequest = "OpticsFoundry_discover";
static const char* DiscoveryAnswerPrefix = "OpticsFoundry sequencer";
static const char* SetNetworkRequest = "OpticsFoundry_set_network";
static const char* SetNetworkAnswerPrefix = "OpticsFoundry set_network";

// IPv4 broadcast addresses (network byte order) of all active interfaces, plus the limited broadcast address
static std::vector<unsigned long> GetBroadcastAddresses() {
	std::vector<unsigned long> addresses;
	addresses.push_back(htonl(INADDR_BROADCAST));
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buffer(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(size);
		result = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
	}
	if (result != NO_ERROR) return addresses;
	for (PIP_ADAPTER_ADDRESSES adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter != nullptr; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp) continue;
		if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
			if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
			unsigned long address = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr;
			unsigned int prefix = unicast->OnLinkPrefixLength;
			if (prefix > 32) continue;
			unsigned long mask = (prefix == 0) ? 0 : htonl(0xFFFFFFFFul << (32 - prefix));
			addresses.push_back(address | ~mask);
		}
	}
#else
	struct ifaddrs* interfaces = nullptr;
	if (getifaddrs(&interfaces) != 0) return addresses;
	for (struct ifaddrs* i = interfaces; i != nullptr; i = i->ifa_next) {
		if ((i->ifa_addr == nullptr) || (i->ifa_addr->sa_family != AF_INET)) continue;
		if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST)) continue;
		if (i->ifa_broadaddr == nullptr) continue;
		addresses.push_back(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
	}
	freeifaddrs(interfaces);
#endif
	return addresses;
}

// the "MAC ..." field identifies a sequencer; used to remove duplicate answers
static std::string MacOf(const std::string& answer) {
	size_t start = answer.find("MAC ");
	if (start == std::string::npos) return answer;
	size_t end = answer.find(';', start);
	return answer.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
}

// Sends request as UDP broadcast to the discovery port on every network interface and collects the answers that start
// with answer_prefix (without line end; duplicates, identified by their MAC field, are removed). If stop_after_first_answer,
// returns as soon as one answer has arrived. Returns false only if the request couldn't be sent at all.
static bool BroadcastRequest(const std::string& request, const char* answer_prefix, double timeout_in_seconds, bool stop_after_first_answer,
		std::vector<std::string>& answers, std::string& error) {
	answers.clear();
	error.clear();
#ifdef _WIN32
	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		error = "WSAStartup failed";
		return false;
	}
#endif
	bool ok = true;
	SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET_HANDLE) {
		error = "can't create UDP socket";
		ok = false;
	}
	if (ok) {
		int enable = 1;
		setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enable), sizeof(enable));
		sockaddr_in local;
		std::memset(&local, 0, sizeof(local));
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port = 0;
		if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
			error = "can't bind UDP socket";
			ok = false;
		}
	}
	if (ok) {
		std::set<unsigned long> sent;
		int sent_ok = 0;
		for (unsigned long address : GetBroadcastAddresses()) {
			if (!sent.insert(address).second) continue;
			sockaddr_in destination;
			std::memset(&destination, 0, sizeof(destination));
			destination.sin_family = AF_INET;
			destination.sin_addr.s_addr = address;
			destination.sin_port = htons(DiscoveryPort);
			if (sendto(s, request.c_str(), static_cast<int>(request.size()), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) > 0) sent_ok++;
		}
		if (sent_ok == 0) {
			error = "couldn't send the UDP broadcast on any network interface";
			ok = false;
		}
	}
	if (ok) {
		std::set<std::string> known;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(timeout_in_seconds * 1000));
		while (true) {
			auto now = std::chrono::steady_clock::now();
			if (now >= deadline) break;
			long long remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
			fd_set read_set;
			FD_ZERO(&read_set);
			FD_SET(s, &read_set);
			timeval timeout;
			timeout.tv_sec = static_cast<long>(remaining_us / 1000000);
			timeout.tv_usec = static_cast<long>(remaining_us % 1000000);
			int ready = select(static_cast<int>(s) + 1, &read_set, nullptr, nullptr, &timeout);
			if (ready <= 0) break;
			char buffer[1024];
			sockaddr_in from;
#ifdef _WIN32
			int from_length = sizeof(from);
#else
			socklen_t from_length = sizeof(from);
#endif
			int received = recvfrom(s, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_length);
			if (received <= 0) continue;
			buffer[received] = 0;
			std::string answer(buffer);
			while (!answer.empty() && ((answer.back() == '\n') || (answer.back() == '\r'))) answer.pop_back();
			if (answer.compare(0, std::strlen(answer_prefix), answer_prefix) != 0) continue;
			if (known.insert(MacOf(answer)).second) answers.push_back(answer);
			if (stop_after_first_answer) break;
		}
	}
	if (s != INVALID_SOCKET_HANDLE) CloseSocketHandle(s);
#ifdef _WIN32
	WSACleanup();
#endif
	return ok;
}

bool DiscoverSequencers(std::vector<std::string>& sequencers, double timeout_in_seconds, std::string& error) {
	return BroadcastRequest(DiscoveryRequest, DiscoveryAnswerPrefix, timeout_in_seconds, /*stop_after_first_answer*/ false, sequencers, error);
}

// "02:4f:46:4c:b5:52", "02-4F-46-4C-B5-52" -> "02:4F:46:4C:B5:52"; false if not a MAC address
static bool NormalizeMac(const std::string& text, std::string& mac) {
	std::string hex;
	for (char c : text) {
		if (std::isxdigit(static_cast<unsigned char>(c))) hex += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		else if ((c != ':') && (c != '-') && (c != ' ')) return false;
	}
	if (hex.size() != 12) return false;
	mac.clear();
	for (size_t i = 0; i < 12; i += 2) {
		if (i > 0) mac += ':';
		mac += hex.substr(i, 2);
	}
	return true;
}

bool SetSequencerNetworkByMAC(const std::string& mac_address, const std::string& settings, double timeout_in_seconds, std::string& answer, std::string& error) {
	answer.clear();
	std::string mac;
	if (!NormalizeMac(mac_address, mac)) {
		error = "invalid MAC address \"" + mac_address + "\" (expected e.g. 02:4F:46:4C:B5:52)";
		return false;
	}
	std::vector<std::string> answers;
	if (!BroadcastRequest(std::string(SetNetworkRequest) + " " + mac + " " + settings, SetNetworkAnswerPrefix, timeout_in_seconds,
			/*stop_after_first_answer*/ true, answers, error)) return false;
	for (const std::string& a : answers) {
		if (a.find("MAC " + mac) == std::string::npos) continue;
		answer = a;
		size_t result = a.find("; ", a.find("MAC "));
		std::string status = (result == std::string::npos) ? a : a.substr(result + 2);
		if (status.rfind("OK", 0) == 0) return true;
		error = "sequencer " + mac + " refused: " + status;
		return false;
	}
	error = "no answer from sequencer " + mac + " (wrong MAC address, sequencer not connected, or firmware older than 2026-11-27)";
	return false;
}
