// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Service::Nx2 {

/**
 * Picks, from present intervals, whether a docked Switch 2 game is told it is handheld (so it
 * renders at its handheld resolution) or docked. Ocarina of Time renders 2560x1440 docked and
 * 1920x1080 handheld and switches live; on an RTX 2060 the docked scenes ran at 26.6 fps and the
 * handheld ones at 39.7 fps, with a tenth of the frames over 100 ms.
 *
 * Every window (2 s of presents) gives a median interval. Docked: windows in a row over 1.2x the
 * game's target interval switch to handheld; if handheld is not 10% faster (the game is not bound
 * by the GPU), it goes back to docked and stays there for a while. Handheld: windows in a row at
 * the target try docked again; a try that turns slow goes back and doubles the wait before the
 * next one. Windows with few presents (loading, menus that stop presenting) count for nothing.
 */
class AutoResolution {
public:
    enum class Change { None, ToHandheld, ToDocked };

    static constexpr std::int64_t WindowUs = 2'000'000;
    static constexpr std::size_t MinPresents = 30;
    static constexpr int SlowWindows = 3;      ///< Docked windows over the slow limit to drop
    static constexpr int ProbeSlowWindows = 2; ///< ... while trying docked again
    static constexpr int CheckWindows = 3;     ///< Handheld windows compared with docked
    static constexpr int GoodWindows = 15;     ///< Handheld windows at the target to try docked
    static constexpr std::int64_t BaseProbeWaitUs = 60'000'000;
    static constexpr std::int64_t MaxProbeWaitUs = 600'000'000;
    static constexpr std::int64_t NotGpuBoundWaitUs = 300'000'000;

    /// One present: its interval from the previous one and the game's target interval.
    Change Present(std::int64_t now_us, std::int64_t frame_us, std::int64_t target_us) {
        if (window_start_us < 0) {
            window_start_us = now_us;
        }
        samples.push_back(frame_us);
        if (now_us - window_start_us < WindowUs) {
            return Change::None;
        }
        const bool enough = samples.size() >= MinPresents;
        std::int64_t median = 0;
        if (enough) {
            auto middle = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2);
            std::nth_element(samples.begin(), middle, samples.end());
            median = *middle;
        }
        samples.clear();
        window_start_us = now_us;
        if (!enough) {
            slow = good = 0;
            return Change::None;
        }
        return Window(now_us, median, target_us);
    }

    /// Why the last switch happened.
    [[nodiscard]] const char* Reason() const {
        return reason;
    }

    [[nodiscard]] bool Handheld() const {
        return handheld;
    }

    /// Back to docked with no history (the setting was turned off or the console mode changed).
    void Reset() {
        *this = AutoResolution{};
    }

private:
    Change Window(std::int64_t now_us, std::int64_t median, std::int64_t target_us) {
        if (settle > 0) {
            // The windows right after a switch hold the game's reallocation of its targets.
            --settle;
            return Change::None;
        }
        if (!handheld) {
            const bool is_slow = median * 5 > target_us * 6;
            slow = is_slow ? slow + 1 : 0;
            good = is_slow ? 0 : good + 1;
            if (probing) {
                if (slow >= ProbeSlowWindows) {
                    probing = false;
                    probe_wait_us = (std::min)(probe_wait_us * 2, MaxProbeWaitUs);
                    return Switch(true, now_us, "the full resolution did not keep up again");
                }
                if (good >= GoodWindows) {
                    probing = false;
                    probe_wait_us = BaseProbeWaitUs;
                }
                return Change::None;
            }
            docked_sum = is_slow ? docked_sum + median : 0;
            if (slow >= SlowWindows && now_us >= docked_hold_until_us) {
                docked_median = docked_sum / slow;
                checking = true;
                check_sum = 0;
                check_count = 0;
                return Switch(true, now_us, "the game runs below its frame rate");
            }
            return Change::None;
        }
        if (checking) {
            check_sum += median;
            if (++check_count < CheckWindows) {
                return Change::None;
            }
            checking = false;
            if (check_sum / check_count * 10 > docked_median * 9) {
                // Not bound by rendering: the lower resolution only costs image quality.
                docked_hold_until_us = now_us + NotGpuBoundWaitUs;
                return Switch(false, now_us, "the lower resolution was not 10% faster");
            }
        }
        const bool at_target = median * 20 <= target_us * 21;
        good = at_target ? good + 1 : 0;
        if (good >= GoodWindows && now_us >= probe_at_us) {
            probing = true;
            return Switch(false, now_us, "the game keeps its frame rate, trying the full resolution");
        }
        return Change::None;
    }

    Change Switch(bool to_handheld, std::int64_t now_us, const char* why) {
        reason = why;
        handheld = to_handheld;
        settle = 1;
        slow = good = 0;
        docked_sum = 0;
        if (to_handheld) {
            probe_at_us = now_us + probe_wait_us;
        }
        return to_handheld ? Change::ToHandheld : Change::ToDocked;
    }

    std::vector<std::int64_t> samples;
    std::int64_t window_start_us = -1;
    bool handheld = false;
    bool probing = false;
    bool checking = false;
    int settle = 0;
    int slow = 0;
    int good = 0;
    std::int64_t docked_sum = 0;
    std::int64_t docked_median = 0;
    std::int64_t check_sum = 0;
    int check_count = 0;
    std::int64_t probe_wait_us = BaseProbeWaitUs;
    std::int64_t probe_at_us = 0;
    std::int64_t docked_hold_until_us = 0;
    const char* reason = "";
};

} // namespace Service::Nx2
