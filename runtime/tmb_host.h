// Host OS services the TMB overrides need (thread CPU times, environment), for Linux and Windows. Compiled
// outside the unity build: <windows.h> clashes with raylib names.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tmb::host
{
    struct ThreadCpu
    {
        uint64_t id = 0;
        std::string name;
        int64_t cpuNs = 0; // CPU time used since the thread started
    };

    // Every thread of this process.
    std::vector<ThreadCpu> threadCpuTimes();

    // A clock of the CPU time of the first thread named `name` (ThreadNaming), -1 if there is none.
    int64_t findThreadCpuClock(std::string_view name);
    // Nanoseconds of CPU time on a clock from findThreadCpuClock, 0 if it cannot be read.
    int64_t readThreadCpuNs(int64_t clock);

    // setenv(name, value, 0): sets the variable unless it is already set.
    void setEnvDefault(const char *name, const char *value);
}
