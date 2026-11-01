// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "parameter_sets.h"

namespace pd
{

    std::vector<uint8_t> ExtractParameterSets(const std::vector<uint8_t> &au, Codec codec)
    {
        std::vector<uint8_t> out;
        const size_t n = au.size();
        auto findStart = [&](size_t from, size_t &scLen)
        {
            for (size_t k = from; k + 3 <= n; ++k)
            {
                if (au[k] || au[k + 1])
                    continue;
                if (au[k + 2] == 1)
                    return scLen = 3, k;
                if (k + 3 < n && au[k + 2] == 0 && au[k + 3] == 1)
                    return scLen = 4, k;
            }
            return scLen = 0, n;
        };
        size_t sc = 0, pos = findStart(0, sc);
        while (pos < n)
        {
            size_t start = pos + sc, nextSc = 0, next = findStart(start, nextSc);
            if (start < n)
            {
                uint8_t h = au[start];
                int t = codec == CodecHEVC ? (h >> 1) & 0x3F : h & 0x1F;
                bool isPs = codec == CodecHEVC ? (t >= 32 && t <= 34) : (t == 7 || t == 8);
                if (isPs)
                {
                    out.insert(out.end(), {0, 0, 0, 1});
                    out.insert(out.end(), au.begin() + start, au.begin() + next);
                }
            }
            pos = next;
            sc = nextSc;
        }
        return out;
    }

} // namespace pd
