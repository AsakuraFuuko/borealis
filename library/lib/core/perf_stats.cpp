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

#include <borealis/core/logger.hpp>
#include <borealis/core/perf_stats.hpp>
#include <borealis/core/time.hpp>
#include <cstdio>
#include <cstring>

#if defined(EVO_TARGET_PS5)
#include <borealis/extern/nanovg/agc/evo_direct_mem.h>
#endif

#if defined(PS5) || defined(__PS5__)
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>
#endif

namespace brls
{

namespace
{
    constexpr Time SAMPLE_INTERVAL_US = 1000000;

    MemoryStats cachedStats;
    Time lastSampleTime = 0;
    bool sampled        = false;

    void sampleDirectMemory(MemoryStats& stats)
    {
#if defined(EVO_TARGET_PS5)
        /* evo_direct_mem tracks the pool itself; total_bytes stays 0 until the AGC
         * video context initialises it, so an uninitialised pool reads as absent. */
        evo_direct_mem_stats_t direct {};
        evo_direct_mem_get_stats(&direct);
        if (direct.total_bytes > 0)
        {
            stats.direct_used  = direct.allocated_bytes;
            stats.direct_total = direct.total_bytes;
            stats.direct_valid = true;
        }
#endif
    }

    void sampleRss(MemoryStats& stats)
    {
#if defined(PS5) || defined(__PS5__)
        /* kinfo_proc carries the current resident set; the title's sandbox may
         * refuse the sysctl, so fall back to getrusage's peak (ru_maxrss, KiB on
         * this libc) which the kernel always exposes to the process itself. */
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
        struct kinfo_proc info;
        std::memset(&info, 0, sizeof(info));
        size_t length = sizeof(info);
        if (sysctl(mib, 4, &info, &length, nullptr, 0) == 0 && length >= sizeof(info) && info.ki_rssize > 0)
        {
            long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize <= 0)
                pageSize = 4096;
            stats.rss         = (std::size_t)info.ki_rssize * (std::size_t)pageSize;
            stats.rss_valid   = stats.rss > 0;
            stats.rss_current = stats.rss_valid;
            return;
        }

        struct rusage usage;
        std::memset(&usage, 0, sizeof(usage));
        if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0)
        {
            stats.rss         = (std::size_t)usage.ru_maxrss * 1024;
            stats.rss_valid   = true;
            stats.rss_current = false;
        }
#endif
    }

    void sample(MemoryStats& stats)
    {
        stats = MemoryStats {};
        sampleDirectMemory(stats);
        sampleRss(stats);
    }
} // namespace

const MemoryStats& getMemoryStats()
{
    Time now = getCPUTimeUsec();
    if (!sampled || now - lastSampleTime >= SAMPLE_INTERVAL_US)
    {
        const bool first = !sampled;
        lastSampleTime   = now;
        sampled          = true;
        sample(cachedStats);

        /* One line per process: the HUD has no room for diagnostics and this
         * is exactly the question a missing counter raises. */
        if (first)
            Logger::info("perf hud: direct={}/{} rss={} source={}", cachedStats.direct_used, cachedStats.direct_total,
                cachedStats.rss, cachedStats.rss_current ? "live" : "peak");
    }
    return cachedStats;
}

std::string formatMemoryStats(const MemoryStats& stats)
{
    std::string text;
    char buffer[96];

    if (stats.direct_valid && stats.direct_total > 0)
    {
        std::snprintf(buffer, sizeof(buffer), "MEM:%.1f/%.0fMB", (double)stats.direct_used / (1024.0 * 1024.0),
            (double)stats.direct_total / (1024.0 * 1024.0));
        text = buffer;
    }

    if (stats.rss_valid && stats.rss > 0)
    {
        std::snprintf(buffer, sizeof(buffer), "%s%s:%.0fMB", text.empty() ? "" : "  ",
            stats.rss_current ? "RSS" : "PEAK", (double)stats.rss / (1024.0 * 1024.0));
        text += buffer;
    }

    return text;
}

} // namespace brls
