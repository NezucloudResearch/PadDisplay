// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#pragma once
#include "protocol.h"

#include <cstdint>
#include <vector>

namespace pd
{

    // Collects VPS/SPS/PPS NAL units (with start codes) from an Annex-B access unit.
    std::vector<uint8_t> ExtractParameterSets(const std::vector<uint8_t> &au, Codec codec);

} // namespace pd