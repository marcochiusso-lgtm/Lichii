// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_auto_resolution.h"

namespace {

using Service::Nx2::AutoResolution;
using Change = AutoResolution::Change;

constexpr std::int64_t Target = 16'667;

struct Run {
    AutoResolution controller;
    std::int64_t now = 0;
    std::vector<Change> changes;

    // Presents for `seconds` at `frame_us` each; returns the changes they caused.
    std::vector<Change> Play(double seconds, std::int64_t frame_us) {
        std::vector<Change> seen;
        const auto end = now + static_cast<std::int64_t>(seconds * 1'000'000);
        while (now < end) {
            now += frame_us;
            if (const Change change = controller.Present(now, frame_us, Target);
                change != Change::None) {
                seen.push_back(change);
            }
        }
        return seen;
    }
};

} // namespace

TEST_CASE("AutoResolution: a docked game below its frame rate goes handheld", "[nx2]") {
    Run run;
    REQUIRE(run.Play(10, 16'667).empty());
    REQUIRE(run.Play(10, 37'000) == std::vector{Change::ToHandheld});
    REQUIRE(run.controller.Handheld());
    // 1080p is faster: it stays there while it is still below the target.
    REQUIRE(run.Play(30, 25'000).empty());
    REQUIRE(run.controller.Handheld());
}

TEST_CASE("AutoResolution: a game not bound by rendering goes back to docked", "[nx2]") {
    Run run;
    REQUIRE(run.Play(10, 37'000) == std::vector{Change::ToHandheld});
    // Handheld is just as slow (CPU bound): back to docked, and docked stays.
    REQUIRE(run.Play(12, 36'000) == std::vector{Change::ToDocked});
    REQUIRE_FALSE(run.controller.Handheld());
    REQUIRE(run.Play(120, 37'000).empty());
}

TEST_CASE("AutoResolution: handheld at the target tries docked, backing off when it fails",
          "[nx2]") {
    Run run;
    REQUIRE(run.Play(10, 37'000) == std::vector{Change::ToHandheld});
    // At the target, but the first try waits for the probe delay (60 s after the drop).
    REQUIRE(run.Play(20, 16'667).empty());
    REQUIRE(run.Play(50, 16'667) == std::vector{Change::ToDocked});
    // Docked is slow again: back to handheld quickly.
    REQUIRE(run.Play(10, 30'000) == std::vector{Change::ToHandheld});
    // The next try waits twice as long (120 s).
    REQUIRE(run.Play(100, 16'667).empty());
    REQUIRE(run.Play(30, 16'667) == std::vector{Change::ToDocked});
    // Docked keeps up now: it stays.
    REQUIRE(run.Play(120, 16'667).empty());
    REQUIRE_FALSE(run.controller.Handheld());
}

TEST_CASE("AutoResolution: loading screens do not count", "[nx2]") {
    Run run;
    // A frame every 500 ms (4 per window) is not a slow game.
    REQUIRE(run.Play(30, 500'000).empty());
    REQUIRE_FALSE(run.controller.Handheld());
}
