/*
    Copyright 2021 wiliwili contributors

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#pragma once

#include <cstddef>
#include <string>

namespace brls
{

/**
 * Memory counters shown next to the FPS counter.
 *
 * The direct pool is the AGC direct-memory region the native renderer keeps
 * NanoVG textures and video staging buffers in (evo_direct_mem); RSS is the
 * process resident set as reported by the kernel.
 */
struct MemoryStats
{
    std::size_t direct_used  = 0;
    std::size_t direct_total = 0;
    std::size_t rss          = 0;
    bool direct_valid        = false;
    bool rss_valid           = false;
    /// True when rss is the live resident set, false when it is ru_maxrss.
    bool rss_current = false;
};

/**
 * Counter snapshot, re-sampled at most once per second. Safe to call from a
 * draw path every frame: the fast path only compares the monotonic clock.
 */
const MemoryStats& getMemoryStats();

/**
 * Formats the counters as "MEM:<used>/<total>MB" plus "RSS:<mb>MB" (live
 * resident set) or "PEAK:<mb>MB" (ru_maxrss), omitting the counters the
 * platform does not expose. Empty when nothing is available.
 */
std::string formatMemoryStats(const MemoryStats& stats);

} // namespace brls
