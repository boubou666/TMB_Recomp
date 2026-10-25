#include "tmb_host.h"

#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <chrono>
#include <thread>
#else
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif

namespace tmb::host
{
#if defined(_WIN32)
    namespace
    {
        // GetThreadTimes only moves at each scheduler tick (15.6 ms); QueryThreadCycleTime counts the TSC cycles the
        // thread ran, turned into nanoseconds with the TSC rate measured once against the performance counter.
        double tscCyclesPerNs()
        {
            static const double rate = []
            {
                LARGE_INTEGER frequency, start, end;
                QueryPerformanceFrequency(&frequency);
                QueryPerformanceCounter(&start);
                const uint64_t tscStart = __rdtsc();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                QueryPerformanceCounter(&end);
                const uint64_t tscEnd = __rdtsc();
                const double ns = static_cast<double>(end.QuadPart - start.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart);
                return ns > 0.0 ? static_cast<double>(tscEnd - tscStart) / ns : 1.0;
            }();
            return rate;
        }

        int64_t cpuNsOf(HANDLE thread)
        {
            ULONG64 cycles = 0;
            if (!QueryThreadCycleTime(thread, &cycles))
                return 0;
            return static_cast<int64_t>(static_cast<double>(cycles) / tscCyclesPerNs());
        }

        // GetThreadDescription (Windows 10 1607+), looked up at run time.
        std::string threadName(HANDLE thread)
        {
            using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
            static const auto getThreadDescription = reinterpret_cast<GetThreadDescriptionFn>(
                reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "GetThreadDescription")));
            if (!getThreadDescription)
                return {};
            PWSTR description = nullptr;
            if (FAILED(getThreadDescription(thread, &description)) || !description)
                return {};
            std::string name;
            for (const wchar_t *c = description; *c; ++c)
                name += static_cast<char>(*c < 0x80 ? *c : '?');
            LocalFree(description);
            return name;
        }

        // visit(id, handle) for each thread of this process until it returns false.
        template <typename Visit>
        void forEachThread(Visit visit)
        {
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return;
            const DWORD process = GetCurrentProcessId();
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            bool more = Thread32First(snapshot, &entry);
            for (; more; more = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != process)
                    continue;
                const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread)
                    continue;
                const bool next = visit(entry.th32ThreadID, thread);
                CloseHandle(thread);
                if (!next)
                    break;
            }
            CloseHandle(snapshot);
        }
    }

    std::vector<ThreadCpu> threadCpuTimes()
    {
        std::vector<ThreadCpu> threads;
        forEachThread([&](DWORD id, HANDLE thread)
                      {
                          threads.push_back({id, threadName(thread), cpuNsOf(thread)});
                          return true; });
        return threads;
    }

    int64_t findThreadCpuClock(std::string_view name)
    {
        DWORD found = 0;
        forEachThread([&](DWORD id, HANDLE thread)
                      {
                          if (threadName(thread) != name)
                              return true;
                          found = id;
                          return false; });
        // A handle of its own, kept open: it is the clock.
        const HANDLE thread = found ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, found) : nullptr;
        return thread ? static_cast<int64_t>(reinterpret_cast<intptr_t>(thread)) : -1;
    }

    int64_t readThreadCpuNs(int64_t clock)
    {
        return clock == -1 ? 0 : cpuNsOf(reinterpret_cast<HANDLE>(static_cast<intptr_t>(clock)));
    }

    void setEnvDefault(const char *name, const char *value)
    {
        if (!std::getenv(name))
            _putenv_s(name, value);
    }
#else
    // utime + stime from /proc/self/task/<tid>/stat.
    std::vector<ThreadCpu> threadCpuTimes()
    {
        std::vector<ThreadCpu> threads;
        const double nsPerTick = 1e9 / static_cast<double>(sysconf(_SC_CLK_TCK));
        std::error_code ec;
        for (const auto &task : std::filesystem::directory_iterator("/proc/self/task", ec))
        {
            std::ifstream stat(task.path() / "stat");
            std::string line;
            std::getline(stat, line);
            const size_t open = line.find('('), close = line.rfind(')');
            if (open == std::string::npos || close == std::string::npos)
                continue;
            std::istringstream rest(line.substr(close + 2));
            std::string field;
            uint64_t utime = 0, stime = 0;
            for (int i = 3; i <= 15 && rest >> field; ++i)
            {
                if (i == 14) utime = std::stoull(field);
                if (i == 15) stime = std::stoull(field);
            }
            threads.push_back({std::stoull(task.path().filename().string()), line.substr(open + 1, close - open - 1),
                               static_cast<int64_t>(static_cast<double>(utime + stime) * nsPerTick)});
        }
        return threads;
    }

    int64_t findThreadCpuClock(std::string_view name)
    {
        std::error_code ec;
        for (const auto &task : std::filesystem::directory_iterator("/proc/self/task", ec))
        {
            std::ifstream comm(task.path() / "comm");
            std::string threadName;
            std::getline(comm, threadName);
            if (threadName != name)
                continue;
            // A thread id makes a clock id (CPUCLOCK_SCHED of that thread).
            const pid_t tid = static_cast<pid_t>(std::stol(task.path().filename().string()));
            return static_cast<int64_t>((~static_cast<clockid_t>(tid) << 3) | 6);
        }
        return -1;
    }

    int64_t readThreadCpuNs(int64_t clock)
    {
        timespec ts{};
        if (clock == -1 || clock_gettime(static_cast<clockid_t>(clock), &ts) != 0)
            return 0;
        return ts.tv_sec * 1000000000ll + ts.tv_nsec;
    }

    void setEnvDefault(const char *name, const char *value)
    {
        setenv(name, value, 0);
    }
#endif
}
