// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/sockets/online_profile.h"
#include "core/internal_network/network.h"

// Droplet's private online server (Reservoir). Off unless a server is set, in the emulator's
// settings (Network: online_server, online_server_ca) or, overriding them, the environment:
//   EDEN_NX2_ONLINE_SERVER=<ipv4>[:<port>]  resolve the redirected host names to <ipv4>, and send
//       connections to <ipv4>:443 to <port> (an unprivileged port on the host). While it is set,
//       no other host name resolves, and connections may only go to loopback, private (RFC 1918)
//       and link-local addresses, so nothing reaches Nintendo or any other public server.
//   EDEN_NX2_ONLINE_HOSTS=<suffix>,...  host names (and their subdomains) to redirect. The default
//       is Nintendo's NPLN hosts, Pia's NAT check servers (nncs1/nncs2) and Pia's monitoring
//       server (connection-quality reports, UDP 34343; g<id>.<env>.p.srv.nintendo.net).
//   EDEN_NX2_ONLINE_CA=<file>  DER or PEM certificate that the ssl service hands out as the
//       system's built-in CA (see ssl/cert_store.cpp), so the game trusts the private server.
//       Without one, the certificate Droplet got from the server before (online_friends.cpp) is
//       used, else <data folder>/online/ca.der (or ca.pem); without any, the ssl service gets it
//       from the server when a game first goes online.
//   EDEN_NX2_ONLINE_FRIENDS=<code>,...  friend codes to add for this run (scripts and tests).
// Each server gets its own friend code, secret and friend list (online_profile.h), sent to the
// server as the account's ID token at login.
namespace Service::Sockets::OnlineRedirect {

constexpr std::string_view DefaultHosts =
    "npln.srv.nintendo.net,npln.nintendo.net,n.n.srv.nintendo.net,p.srv.nintendo.net";

// How often a game tried to go online while online play was off (nifm reported no internet, or a
// server name was refused). The windowed app tells the player where to turn it on.
inline std::atomic<u32> online_off_attempts{0};

inline void NoteOnlineOffAttempt() {
    if (online_off_attempts.fetch_add(1, std::memory_order_relaxed) == 0) {
        LOG_WARNING(Network, "A game tried to go online, but online play is off: turn on \"Play "
                             "online on a private server\" in Configure > System > Network");
    }
}

// How often a game set up a secure connection with a server set but without its certificate,
// which Droplet couldn't get from the server either (ssl/cert_store.cpp): the game can't trust
// the server and stops with an error (Mario Kart World: 2321-4992). The windowed app tells the
// player what to check.
inline std::atomic<u32> missing_ca_attempts{0};

inline void NoteMissingCaAttempt() {
    if (missing_ca_attempts.fetch_add(1, std::memory_order_relaxed) == 0) {
        LOG_WARNING(Network, "A game tried to go online, but Droplet couldn't get the server's "
                             "certificate: check the server address, or pick the certificate "
                             "(ca.der, from the server's status page) in Multiplayer > Online Play "
                             "Settings");
    }
}

struct Config {
    bool enabled = false;
    std::array<u8, 4> ip{};
    u16 port = 443;
    std::vector<std::string> suffixes;
};

// The environment wins over the emulator's settings (scripts and tests).
inline std::string FromEnvOr(const char* name, const std::string& setting) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string{value} : setting;
}

inline std::string ServerAddress() {
    return FromEnvOr("EDEN_NX2_ONLINE_SERVER", Settings::values.online_enabled.GetValue()
                                                   ? Settings::values.online_server.GetValue()
                                                   : std::string());
}

inline std::filesystem::path OnlineDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "online";
}

inline std::string ServerCa() {
    // Found whether online play is on or not: the certificate store is made when a game starts,
    // and turning online play on mid-game must find the server's certificate there.
    std::string path = FromEnvOr("EDEN_NX2_ONLINE_CA", Settings::values.online_server_ca.GetValue());
    if (path.empty()) {
        const std::string server =
            FromEnvOr("EDEN_NX2_ONLINE_SERVER", Settings::values.online_server.GetValue());
        std::vector<std::filesystem::path> found{OnlineDir() / "ca.der", OnlineDir() / "ca.pem"};
        if (!server.empty()) {
            found.insert(found.begin(), SavedServerCa(server));
        }
        for (const auto& file : found) {
            std::error_code ec;
            if (std::filesystem::exists(file, ec)) {
                return Common::FS::PathToUTF8String(file);
            }
        }
    }
    return path;
}

// The account's ID token while a server is set: this server's profile (friend code, secret, the
// codes added and removed) and the running game's build, read by the server at sign-in
// (npln_stub.py, parse_identity). Servers keep friendships across sign-ins and only drop the
// removed codes; the build (the main module's build ID in hex) is the game's exact version.
inline std::string IdentityToken(std::string_view build = {}) {
    const std::string server = ServerAddress();
    const auto profile = server.empty() ? std::nullopt : LoadProfile(server, true);
    if (!profile) {
        return {};
    }
    std::vector<std::string> friends = profile->friends;
    if (const char* extra = std::getenv("EDEN_NX2_ONLINE_FRIENDS"); extra != nullptr) {
        for (const std::string& code : ParseCodes(extra)) {
            if (code != profile->code &&
                std::find(friends.begin(), friends.end(), code) == friends.end()) {
                friends.push_back(code);
            }
        }
    }
    const auto join = [](const std::vector<std::string>& codes) {
        std::string out;
        for (const std::string& code : codes) {
            out += (out.empty() ? "" : ",") + code;
        }
        return out;
    };
    std::string token = "ounce2dn-online-v1;code=" + profile->code + ";secret=" + profile->secret;
    if (!build.empty()) {
        token += ";build=" + std::string{build};
    }
    token += ";friends=" + join(friends);
    if (!profile->removed.empty()) {
        token += ";removed=" + join(profile->removed);
    }
    return token;
}

inline Config Parse(const std::string& server_setting) {
    Config c;
    const char* server = server_setting.c_str();
    if (*server == '\0') {
        return c;
    }
    unsigned a = 0, b = 0, d = 0, e = 0, port = 443;
    const int fields = std::sscanf(server, "%u.%u.%u.%u:%u", &a, &b, &d, &e, &port);
    if (fields < 4) {
        // A host name ("raiders.example.org:8443"), looked up once through the host's DNS. Never a
        // Nintendo name: the redirect would then send the game to Nintendo.
        std::string name{server};
        port = 443;
        if (const size_t colon = name.rfind(':'); colon != std::string::npos) {
            port = static_cast<unsigned>(std::strtoul(name.c_str() + colon + 1, nullptr, 10));
            name.resize(colon);
        }
        std::string lower = name;
        for (char& ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (name.empty() || port == 0 || port > 65535 || lower.find("nintendo") != std::string::npos) {
            LOG_CRITICAL(Network, "Online server {} is not <ipv4>[:<port>] or <name>[:<port>]; "
                                  "online play is off", server);
            return c;
        }
        const auto found = Network::GetAddressInfo(name, std::nullopt);
        const auto* addresses = std::get_if<std::vector<Network::AddrInfo>>(&found);
        if (addresses == nullptr || addresses->empty()) {
            LOG_CRITICAL(Network, "Online server {}: {} has no address; online play is off", server,
                         name);
            return c;
        }
        const auto& ip = addresses->front().addr.ip;
        a = ip[0], b = ip[1], d = ip[2], e = ip[3];
        LOG_INFO(Network, "Online server {} is {}.{}.{}.{}", name, a, b, d, e);
    } else if (a > 255 || b > 255 || d > 255 || e > 255 || port == 0 || port > 65535) {
        LOG_CRITICAL(Network, "Online server {} is not <ipv4>[:<port>]; online play is off", server);
        return c;
    }
    c.enabled = true;
    c.ip = {static_cast<u8>(a), static_cast<u8>(b), static_cast<u8>(d), static_cast<u8>(e)};
    c.port = static_cast<u16>(port);
    const char* hosts = std::getenv("EDEN_NX2_ONLINE_HOSTS");
    std::string_view list =
        hosts != nullptr && *hosts != '\0' ? std::string_view{hosts} : DefaultHosts;
    while (!list.empty()) {
        const size_t comma = list.find(',');
        const std::string_view item = list.substr(0, comma);
        if (!item.empty()) {
            c.suffixes.emplace_back(item);
        }
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    LOG_WARNING(Network, "Online redirect: {} -> {}.{}.{}.{}, port 443 -> {}",
                hosts ? std::string_view{hosts} : DefaultHosts, a, b, d, e, c.port);
    if (const auto profile = LoadProfile(server_setting, true)) {
        LOG_INFO(Network, "Online: your friend code on this server is {}; friends added: {} ({})",
                 FormatCode(profile->code), profile->friends.size(), ProfilesDir().string());
    }
    return c;
}

// The redirect for the current server setting (read again when it changes between games).
inline const Config& Get() {
    static std::mutex mutex;
    static std::string current = "\x01";
    static Config config;
    const std::string server = ServerAddress();
    std::scoped_lock lock{mutex};
    if (server != current) {
        current = server;
        config = Parse(server);
    }
    return config;
}

inline bool IsRedirectedHost(std::string_view host) {
    // A resolver may be handed a trailing dot (an absolute name).
    if (!host.empty() && host.back() == '.') {
        host.remove_suffix(1);
    }
    for (const std::string& suffix : Get().suffixes) {
        if (host == suffix || (host.size() > suffix.size() && host.ends_with(suffix) &&
                               host[host.size() - suffix.size() - 1] == '.')) {
            return true;
        }
    }
    return false;
}

// Destinations a game may connect or send to while the redirect is on.
// Where a UDP datagram may go in online mode: the TCP rules below plus other players' public
// addresses, so games reach each other directly over the internet as on a Switch (Pia's peer
// traffic is UDP). Nintendo stays out by name: only the redirected names resolve, to the server.
inline bool IsAllowedDestination(const std::array<u8, 4>& ip);
inline bool IsAllowedPeerDestination(const std::array<u8, 4>& ip) {
    const bool unicast = ip[0] != 0 && ip[0] != 127 && ip[0] < 224;
    return unicast || IsAllowedDestination(ip);
}

inline bool IsAllowedDestination(const std::array<u8, 4>& ip) {
    const Config& c = Get();
    // 100.64.0.0/10 is shared address space: mesh VPNs (Tailscale) give each PC one, so friends
    // in other places reach each other's games through the mesh. Not routable on the internet.
    return ip == c.ip || ip[0] == 127 || ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) ||
           (ip[0] == 192 && ip[1] == 168) || (ip[0] == 169 && ip[1] == 254) ||
           (ip[0] == 100 && (ip[1] & 0xc0) == 64) ||
           ip == std::array<u8, 4>{255, 255, 255, 255} || ip == std::array<u8, 4>{};
}

} // namespace Service::Sockets::OnlineRedirect
