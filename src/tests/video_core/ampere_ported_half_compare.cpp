// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere packed half comparisons preserve all register-form controls",
          "[video_core][ampere][fire_emblem]") {
    for (u32 cmp = 0; cmp < 16; ++cmp) {
        for (u32 combine = 0; combine < 3; ++combine) {
            for (u32 swizzle = 0; swizzle < 4; ++swizzle) {
                for (u32 flags = 0; flags < 64; ++flags) {
                    const u32 a_neg = flags & 1, a_abs = (flags >> 1) & 1;
                    const u32 b_neg = (flags >> 2) & 1, b_abs = (flags >> 3) & 1;
                    const u32 bf = (flags >> 4) & 1, ftz = (flags >> 5) & 1;
                    const std::array<Instruction, 2> code{{
                        {{0x02017233, 3U | (swizzle << 28) | (b_abs << 30) | (b_neg << 31),
                          (combine << 5) | (bf << 7) | (a_neg << 8) | (a_abs << 9) |
                          (swizzle << 10) | (cmp << 12) | (ftz << 16) | (10U << 23), 0}},
                        {{0x794d, 0, 0x03800000, 0}},
                    }};
                    const auto lowered = LowerToMaxwell(code);
                    REQUIRE(lowered);
                    const u64 expected = 0x5d18000000070001ULL | (2ULL << 8) | (3ULL << 20) |
                        (u64{swizzle} << 28) | (u64{b_abs} << 30) | (u64{b_neg} << 31) |
                        (u64{cmp} << 35) | (10ULL << 39) | (u64{a_neg} << 43) |
                        (u64{a_abs} << 44) | (u64{combine} << 45) | (u64{swizzle} << 47) |
                        (u64{bf} << 49) | (u64{ftz} << 50);
                    REQUIRE(lowered.instructions.front() == expected);
                }
            }
        }
    }
    std::array<Instruction, 2> invalid{{{{0x02017233, 3, 3U << 5, 0}}, {{0x794d, 0, 0x03800000, 0}}}};
    REQUIRE_FALSE(LowerToMaxwell(invalid));
}

TEST_CASE("Ampere HSETP2 reads and validates constant-buffer operands", "[video_core][ampere]") {
    constexpr Instruction Exit{{0x794d, 0, 0x03800000, 0}};
    Instruction compare{{0x02007634, (0xfcU << 6) | (7U << 22) | (2U << 28) | (3U << 30),
                         (5U << 12) | (7U << 17) | (7U << 20) | (7U << 23), 0}};
    const auto lowered = LowerToMaxwell(std::array{compare, Exit});
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() >= 2);
    const u64 load = lowered.instructions[0];
    CHECK((load >> 52) == 0x4c9);
    CHECK(((load >> 34) & 31) == 7);
    CHECK(((load >> 20) & 0x3fff) == 0x3f);
    const u64 half_compare = lowered.instructions[1];
    CHECK((half_compare >> 52) == 0x5d2);
    CHECK(((half_compare >> 20) & 0xff) == 250);
    CHECK(((half_compare >> 28) & 3) == 2);
    CHECK(((half_compare >> 30) & 3) == 3);

    auto invalid = compare;
    invalid.words[1] |= 1U << 6; // Unaligned constant-buffer offset.
    CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    invalid = compare;
    invalid.words[1] = (invalid.words[1] & ~(31U << 22)) | (19U << 22);
    CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));

    constexpr Instruction UniformMarker{{0x0000fc02, 0, 0, 0}};
    compare.words[0] = (compare.words[0] & 0x00ffffffU) | (240U << 24);
    CHECK(LowerToMaxwell(std::array{UniformMarker, compare, Exit}));
}
