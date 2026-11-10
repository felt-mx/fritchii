// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon::HdrOutput {

/// Whether the host swapchain shows HDR (HDR10 or scRGB). Written by the Vulkan presenter when it
/// creates a swapchain, read by vi2 to answer the game's display HDR queries.
inline std::atomic<bool> g_host_active{false};

/// The display HDR status a Switch 2 game is told (nn::vi2 104/105).
struct Status {
    bool hdr{};
    u16 peak_luminance{};
    u16 paper_white{};

    bool operator==(const Status&) const = default;
};

inline Status CurrentStatus() {
    const bool hdr = Settings::values.hdr_output.GetValue() &&
                     g_host_active.load(std::memory_order_relaxed);
    return Status{
        .hdr = hdr,
        // 400 nits: a typical SDR panel, what vi2 reported before HDR output existed.
        .peak_luminance =
            static_cast<u16>(hdr ? Settings::values.hdr_peak_luminance.GetValue() : 400),
        .paper_white = static_cast<u16>(Settings::values.hdr_paper_white.GetValue()),
    };
}

} // namespace VideoCommon::HdrOutput
