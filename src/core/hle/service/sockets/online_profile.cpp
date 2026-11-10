// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/sockets/online_profile.h"

namespace Service::Sockets::OnlineRedirect {

namespace {

std::mutex profile_mutex;

std::string ProfileKey(const std::string& server) {
    std::string key;
    for (const char ch : server) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        key += (std::isalnum(static_cast<unsigned char>(lower)) || lower == '.' || lower == '-')
                   ? lower
                   : '_';
    }
    return key.empty() ? "default" : key;
}

std::filesystem::path ProfilePath(const std::string& server) {
    return ProfilesDir() / (ProfileKey(server) + ".txt");
}

} // namespace

std::filesystem::path SavedServerCa(const std::string& server) {
    return ProfilesDir() / (ProfileKey(server) + ".ca.der");
}

namespace {

std::string NewCode() {
    std::random_device device;
    std::uniform_int_distribution<unsigned long long> digits(1, 999'999'999'999ULL);
    char code[16];
    std::snprintf(code, sizeof(code), "%012llu", digits(device));
    return code;
}

std::string JoinCodes(const std::vector<std::string>& codes) {
    std::string out;
    for (const std::string& code : codes) {
        out += (out.empty() ? "" : ",") + code;
    }
    return out;
}

bool HasProfiles() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(ProfilesDir(), ec)) {
        if (entry.path().extension() == ".txt") {
            return true;
        }
    }
    return false;
}

// Before profiles, each emulator folder had one friend code for every server
std::optional<Profile> LegacyIdentity(const std::string& server) {
    std::ifstream in(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "online" / "identity.txt");
    std::stringstream text;
    text << in.rdbuf();
    auto profile = ParseProfile(text.str());
    if (!profile) {
        return std::nullopt;
    }
    profile->server = server;
    for (const std::string& code : ParseCodes(Settings::values.online_friends.GetValue())) {
        AddFriend(*profile, code);
    }
    return profile;
}

// <base>/<name>/profiles. Profiles from before the rename to Droplet (<base>/<old_name>) move
// there the first time.
std::filesystem::path UserProfilesDir(const std::filesystem::path& base, const char* old_name,
                                      const char* name) {
    const auto dir = base / name / "profiles";
    static std::once_flag moved;
    std::call_once(moved, [&] {
        const auto old_dir = base / old_name / "profiles";
        std::error_code ec;
        if (std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(old_dir, ec)) {
            return;
        }
        std::filesystem::create_directories(dir.parent_path(), ec);
        std::filesystem::rename(old_dir, dir, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy(old_dir, dir, std::filesystem::copy_options::recursive, ec);
        }
        if (ec) {
            LOG_ERROR(Network, "Online: could not move the profiles in {} to {}: {}",
                      old_dir.string(), dir.string(), ec.message());
        } else {
            LOG_INFO(Network, "Online: moved the profiles in {} to {}", old_dir.string(), dir.string());
        }
    });
    return dir;
}

} // namespace

std::filesystem::path ProfilesDir() {
    if (const char* dir = std::getenv("EDEN_NX2_ONLINE_PROFILES"); dir != nullptr && *dir != '\0') {
        return std::filesystem::path{dir};
    }
#ifdef _WIN32
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata != nullptr && *appdata != L'\0') {
        return UserProfilesDir(std::filesystem::path{appdata}, "Ounce2dn-online", "Droplet-online");
    }
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data == '/') {
        return UserProfilesDir(std::filesystem::path{data}, "ounce2dn-online", "droplet-online");
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return UserProfilesDir(std::filesystem::path{home} / ".local" / "share", "ounce2dn-online",
                               "droplet-online");
    }
#endif
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "online" / "profiles";
}

std::optional<Profile> LoadProfile(const std::string& server, bool create) {
    std::scoped_lock lock{profile_mutex};
    std::ifstream in(ProfilePath(server));
    if (in) {
        std::stringstream text;
        text << in.rdbuf();
        if (auto profile = ParseProfile(text.str())) {
            profile->server = server;
            return profile;
        }
        LOG_ERROR(Network, "Online: {} is damaged; making a new friend code",
                  ProfilePath(server).string());
    }
    std::optional<Profile> profile;
    if (!HasProfiles()) {
        profile = LegacyIdentity(server);
    }
    if (!create) {
        return profile; // the friend code the first sign-in will keep, if there is one
    }
    if (!profile) {
        profile = Profile{server, NewCode(), NewSecret(), {}, {}};
    }
    std::error_code ec;
    std::filesystem::create_directories(ProfilesDir(), ec);
    const auto path = ProfilePath(server);
    std::ofstream out(path, std::ios::trunc);
    out << ProfileText(*profile);
    out.close();
    if (!out) {
        LOG_ERROR(Network, "Online: could not save the friend code to {}", path.string());
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write, ec);
    return profile;
}

bool SaveProfile(const Profile& profile) {
    std::scoped_lock lock{profile_mutex};
    std::error_code ec;
    std::filesystem::create_directories(ProfilesDir(), ec);
    const auto path = ProfilePath(profile.server);
    auto temp = path;
    temp += ".new";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << ProfileText(profile);
        if (!out) {
            LOG_ERROR(Network, "Online: could not save {}", path.string());
            return false;
        }
    }
    std::filesystem::permissions(temp, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write, ec);
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        LOG_ERROR(Network, "Online: could not save {}: {}", path.string(), ec.message());
        return false;
    }
    return true;
}

std::string ProfileText(const Profile& profile) {
    return "server=" + profile.server + "\ncode=" + profile.code + "\nsecret=" + profile.secret +
           "\nfriends=" + JoinCodes(profile.friends) + "\nremoved=" + JoinCodes(profile.removed) +
           "\n";
}

std::optional<Profile> ParseProfile(std::string_view text) {
    Profile profile;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, equals), value = line.substr(equals + 1);
        if (key == "server") {
            profile.server = value;
        } else if (key == "code") {
            profile.code = value;
        } else if (key == "secret") {
            profile.secret = value;
        } else if (key == "friends") {
            profile.friends = ParseCodes(value);
        } else if (key == "removed") {
            profile.removed = ParseCodes(value);
        }
    }
    const auto codes = ParseCodes(profile.code);
    if (codes.size() != 1 || profile.secret.size() < 16) {
        return std::nullopt;
    }
    profile.code = codes.front();
    return profile;
}

std::string NewSecret() {
    std::random_device device;
    char secret[40];
    std::snprintf(secret, sizeof(secret), "%08x%08x%08x%08x", device(), device(), device(),
                  device());
    return secret;
}

std::vector<std::string> ParseCodes(std::string_view text) {
    std::vector<std::string> codes;
    std::string digits;
    const auto flush = [&] {
        if (digits.size() == 12 && digits != "000000000000" &&
            std::find(codes.begin(), codes.end(), digits) == codes.end()) {
            codes.push_back(digits);
        }
        digits.clear();
    };
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            digits += ch;
        } else if (ch == ',' || ch == ';' || ch == '\n' || std::isalpha(static_cast<unsigned char>(ch))) {
            // A letter starts the next code: "SW-1234-5678-9012 SW-2345-6789-0123"
            flush();
        }
    }
    flush();
    return codes;
}

std::string FormatCode(const std::string& code) {
    return code.size() == 12
               ? "SW-" + code.substr(0, 4) + '-' + code.substr(4, 4) + '-' + code.substr(8)
               : code;
}

void AddFriend(Profile& profile, const std::string& code) {
    if (code == profile.code) {
        return;
    }
    std::erase(profile.removed, code);
    if (std::find(profile.friends.begin(), profile.friends.end(), code) == profile.friends.end()) {
        profile.friends.push_back(code);
    }
}

void RemoveFriend(Profile& profile, const std::string& code) {
    std::erase(profile.friends, code);
    if (std::find(profile.removed.begin(), profile.removed.end(), code) == profile.removed.end()) {
        profile.removed.push_back(code);
    }
}

} // namespace Service::Sockets::OnlineRedirect
