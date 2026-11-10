// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/hle/service/sockets/online_profile.h"

// Friend requests on a private online server: Reservoir's friends API, HTTPS on the online-play
// port + 1. Only the server's certificate is trusted there, as in a game's sign-in, and the
// profile's friend code and secret sign each call. The same port hands out that certificate, so
// players don't need to fetch it themselves. The calls block (a few seconds at most), so the
// settings page makes them off its thread.
namespace Service::Sockets::OnlineRedirect {

struct FriendEntry {
    std::string code;
    std::string name; // the player's name in the game, as the server last saw it
    bool online = false;
};

struct FriendLists {
    std::vector<FriendEntry> friends;
    std::vector<FriendEntry> received; // requests to this player, newest first
    std::vector<FriendEntry> sent;     // requests this player sent that aren't answered yet
    std::string error;                 // why the server didn't answer, or refused the action
    bool reached = false;              // the server answered (an error then is its refusal)
};

// Gets the server's certificate from the server and keeps it (SavedServerCa), for games and for
// the calls below: an empty string, or why not. Nothing vouches for this first download (the
// certificate is what would); after it, the kept certificate is the check, as with SSH.
std::string FetchServerCa(const std::string& server);

// The player's friends and requests. `ca` is the server's certificate (DER or PEM file); empty
// takes the one online play uses.
FriendLists FetchFriends(const Profile& profile, const std::string& ca = {});
// "request", "accept", "decline", "cancel" or "remove" for `code`; the lists as they are after.
FriendLists FriendAction(const Profile& profile, const std::string& action, const std::string& code,
                         const std::string& ca = {});

} // namespace Service::Sockets::OnlineRedirect
