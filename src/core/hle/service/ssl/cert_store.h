// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <span>
#include <vector>

#include "core/hle/result.h"
#include "core/hle/service/ssl/ssl_types.h"

namespace Core {
class System;
}

namespace Service::SSL {

class CertStore {
public:
    explicit CertStore(Core::System& system);
    ~CertStore();

    Result GetCertificates(u32* out_num_entries, std::span<u8> out_data,
                           std::span<const CaCertificateId> certificate_ids);
    Result GetCertificateBufSize(u32* out_size, u32* out_num_entries,
                                 std::span<const CaCertificateId> certificate_ids);

private:
    template <typename F>
    void ForEachCertificate(std::span<const CaCertificateId> certs, F&& f);

    void EnsureOnlineCa();

private:
    struct Certificate {
        TrustedCertStatus status;
        std::vector<u8> der_data;
    };

    std::map<CaCertificateId, Certificate> m_certs;
    // A private server is set but its CA isn't here yet: fetched when the game first goes online.
    bool m_online_ca_missing = false;
    std::mutex m_online_ca_mutex;
    std::chrono::steady_clock::time_point m_next_ca_fetch{};
};

} // namespace Service::SSL
