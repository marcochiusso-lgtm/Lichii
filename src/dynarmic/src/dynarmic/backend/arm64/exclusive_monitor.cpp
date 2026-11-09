// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/interface/exclusive_monitor.h"

#include <algorithm>
#include <atomic>

#include "common/assert.h"

namespace Dynarmic {

ExclusiveMonitor::ExclusiveMonitor(std::size_t processor_count)
        : exclusive_addresses(processor_count, INVALID_EXCLUSIVE_ADDRESS), exclusive_values(processor_count) {}

size_t ExclusiveMonitor::GetProcessorCount() const {
    return exclusive_addresses.size();
}

void ExclusiveMonitor::Lock() {
    lock.Lock();
}

void ExclusiveMonitor::Unlock() {
    lock.Unlock();
}

bool ExclusiveMonitor::CheckAndClear(std::size_t processor_id, VAddr address) {
    const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

#if defined(ARCHITECTURE_arm64) && defined(_MSC_VER)
    // MSVC has no __atomic builtins, but its standard library provides std::atomic_ref.
    std::atomic_ref<VAddr> own{exclusive_addresses[processor_id]};
    VAddr expected = masked_address;
    if (own.load(std::memory_order_relaxed) != masked_address ||
        !own.compare_exchange_strong(expected, INVALID_EXCLUSIVE_ADDRESS, std::memory_order_acq_rel)) {
        return false;
    }
    for (VAddr& other_address : exclusive_addresses) {
        std::atomic_ref<VAddr> other{other_address};
        if (other.load(std::memory_order_relaxed) == masked_address) {
            expected = masked_address;
            other.compare_exchange_strong(expected, INVALID_EXCLUSIVE_ADDRESS,
                                          std::memory_order_acq_rel);
        }
    }
    return true;
#elif defined(ARCHITECTURE_arm64)
    // std::atomic_ref is missing from Android's libc++, so use the equivalent compiler builtins.
    VAddr& own = exclusive_addresses[processor_id];
    VAddr expected = masked_address;
    if (__atomic_load_n(&own, __ATOMIC_RELAXED) != masked_address ||
        !__atomic_compare_exchange_n(&own, &expected, INVALID_EXCLUSIVE_ADDRESS, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;
    }
    for (VAddr& other_address : exclusive_addresses) {
        if (__atomic_load_n(&other_address, __ATOMIC_RELAXED) == masked_address) {
            expected = masked_address;
            __atomic_compare_exchange_n(&other_address, &expected, INVALID_EXCLUSIVE_ADDRESS, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    }
    return true;
#endif

    Lock();
    if (exclusive_addresses[processor_id] != masked_address) {
        Unlock();
        return false;
    }

    for (VAddr& other_address : exclusive_addresses) {
        if (other_address == masked_address) {
            other_address = INVALID_EXCLUSIVE_ADDRESS;
        }
    }
    return true;
}

void ExclusiveMonitor::Clear() {
#ifdef ARCHITECTURE_arm64
    for (VAddr& address : exclusive_addresses) {
#ifdef _MSC_VER
        std::atomic_ref<VAddr>{address}.store(INVALID_EXCLUSIVE_ADDRESS, std::memory_order_release);
#else
        __atomic_store_n(&address, INVALID_EXCLUSIVE_ADDRESS, __ATOMIC_RELEASE);
#endif
    }
#else
    Lock();
    std::fill(exclusive_addresses.begin(), exclusive_addresses.end(), INVALID_EXCLUSIVE_ADDRESS);
    Unlock();
#endif
}

void ExclusiveMonitor::ClearProcessor(std::size_t processor_id) {
#if defined(ARCHITECTURE_arm64) && defined(_MSC_VER)
    std::atomic_ref<VAddr>{exclusive_addresses[processor_id]}.store(INVALID_EXCLUSIVE_ADDRESS, std::memory_order_release);
#elif defined(ARCHITECTURE_arm64)
    __atomic_store_n(&exclusive_addresses[processor_id], INVALID_EXCLUSIVE_ADDRESS, __ATOMIC_RELEASE);
#else
    Lock();
    exclusive_addresses[processor_id] = INVALID_EXCLUSIVE_ADDRESS;
    Unlock();
#endif
}

}  // namespace Dynarmic
