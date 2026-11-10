// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>

#include "common/common_types.h"

namespace Common::FS::DirectStorage {

/// Reads smaller than this stay on std::fread: DirectStorage pays off on larger (or batched)
/// reads, and the per-call setup is not worth it below this size.
inline constexpr size_t MinDirectStorageReadSize = 64 * 1024;

/// True when the DirectStorage runtime (dstorage.dll) was loaded and a queue was created.
[[nodiscard]] bool IsAvailable();

/// Reads `size` bytes at `offset` from `path` into `dst`, waiting for completion. Returns false
/// on any failure (runtime missing, open failed, request failed); the caller then falls back
/// to a regular read.
[[nodiscard]] bool ReadFileAt(const std::filesystem::path& path, u64 offset, void* dst,
                              size_t size);

} // namespace Common::FS::DirectStorage
