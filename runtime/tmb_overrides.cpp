// Game-specific runtime setup for Twisted Metal: Black (PAL, SCES-50360).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "Stubs/MPEG.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_draw_estimate.h"
#include "runtime/ps2_memory.h"
#include "runtime/sampling_profiler_gate.h"
#include "tmb_host.h"
#include <string>
#include <thread>
#include <atomic>
#include <ctime>
#include <mutex>
#include <chrono>
#include <algorithm>

#include <cmath>
#include <cstdio>
#include <vector>
#include <cstring>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <iostream>

namespace ps2_stubs
{
    std::string mpegDebugSummary(); // Stubs/MPEG.cpp
}

namespace tmb
{
    extern std::atomic<bool> g_rainLiftOff; // runtime/vu1/tmb_vu1_rain.cpp
    extern std::atomic<uint64_t> g_rainLiftedStrips;
}

namespace tmb::native
{
    void onFlushObjQ(const uint8_t *rdram);
    void onFrameEnd();
    bool enabled();
}

namespace
{
    constexpr uint32_t kTmbPalEntry = 0x00100008u;

    void timelineEvent(char type, uint32_t arg = 0); // TMB_TRACE_TIMELINE

    // The game reads raw sectors with sceCdRead (LBN 0x10, the primary volume descriptor,
    // right after boot), which the file-backed cdrom0: cannot serve. Give the CD device the
    // disc image itself: TMB_ISO, else the first .iso found walking up from the ELF folder
    // (game/ -> repository root).
    std::filesystem::path findDiscImage(const std::filesystem::path &elfDirectory)
    {
        if (const char *configured = std::getenv("TMB_ISO"); configured && configured[0] != '\0')
        {
            return configured;
        }
        std::filesystem::path dir = elfDirectory;
        for (int depth = 0; depth < 4 && !dir.empty(); ++depth, dir = dir.parent_path())
        {
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (!entry.is_regular_file(ec))
                {
                    continue;
                }
                std::string ext = entry.path().extension().string();
                for (char &c : ext)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (ext == ".iso")
                {
                    return entry.path();
                }
            }
            if (dir == dir.root_path())
            {
                break;
            }
        }
        return {};
    }

    // Movie skip. The player (Sony's ezmpeg sample) aborts a movie with videoDecAbort, then
    // videoDecFlush appends a sequence_end_code to the IPU ring and waits until the decode
    // thread reaches it. With libmpeg HLE'd that code never reaches the host decoder, so
    // sceMpegIsEnd stays false and updateMpegMovieDecoding waits forever. Tell the HLE the
    // playback is over at the abort; the VideoDec starts with its sceMpeg, so a0 is both.
    constexpr uint32_t kVideoDecAbort = 0x001FF920u;

    void videoDecAbortOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t videoDec = getRegU32(ctx, 4);
        videoDecAbort__FP8VideoDec_0x1ff920(rdram, ctx, runtime);
        ps2_stubs::abortMpegPlayback(runtime, videoDec);
    }

    // The movie player's switchThread is RotateThreadReadyQueue(1): Sony's ezmpeg sample runs its main loop at
    // priority 1, so the call lets the decode threads (priority 1) run. TMB's main thread is at priority 0, where
    // the rotation does nothing. The player's waits mostly block on something else, but after a skip
    // (videoDecAbort) updateMpegMovieDecoding loops on switchThread alone until the decode thread acknowledges
    // the abort, which it never gets to do: the game hung on a skipped copyright movie. A priority-0 caller drops
    // to priority 1 for the rotation, so the priority-1 threads run once, and gets its priority back when it is
    // scheduled again, through kSwitchThreadReturn (padding after readMpeg, never executed).
    constexpr uint32_t kSwitchThread = 0x001E06D0u;
    constexpr uint32_t kSwitchThreadReturn = 0x001E06CCu;
    constexpr int kMoviePriority = 1;
    std::unordered_map<int, std::pair<int, uint32_t>> g_switchThreadSaved; // thread -> (priority, return address)

    void switchThreadOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = runtime->eeScheduler();
        const GuestThread *self = ee.currentThread();
        if (!self || self->currentPriority >= kMoviePriority)
        {
            switchThread__Fv_0x1e06d0(rdram, ctx, runtime);
            return;
        }
        const int id = ee.currentThreadId();
        int oldPriority = 0;
        if (ee.changePriority(id, kMoviePriority, false, oldPriority) != 0) // KE_OK
        {
            switchThread__Fv_0x1e06d0(rdram, ctx, runtime);
            return;
        }
        g_switchThreadSaved[id] = {oldPriority, getRegU32(ctx, 31)};
        ctx->pc = kSwitchThreadReturn;
        SET_GPR_U32(ctx, 2, static_cast<uint32_t>(ee.rotateReadyQueue(kMoviePriority, false)));
        ee.transferIfRequested(false);
    }

    void switchThreadReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &ee = runtime->eeScheduler();
        const auto saved = g_switchThreadSaved.find(ee.currentThreadId());
        if (saved == g_switchThreadSaved.end())
            return;
        const auto [priority, ret] = saved->second;
        g_switchThreadSaved.erase(saved);
        ctx->pc = ret;
        int oldPriority = 0;
        ee.changePriority(0, priority, false, oldPriority);
        ee.transferIfRequested(false);
    }

    // TMB_TRACE_MOVIE=1: log the movie player's thread teardown (temporary bring-up aid).
    constexpr uint32_t kTermAll = 0x001E0948u;

    void termAllTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const auto readGuest = [&](uint32_t addr) { return *reinterpret_cast<const int32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const int ids[2] = {readGuest(gp - 0x321Cu), readGuest(gp - 0x3218u)};
        termAll__Fv_0x1e0948(rdram, ctx, runtime);
        for (int id : ids)
        {
            const GuestThread *t = runtime->eeScheduler().thread(id);
            std::cerr << "[tmb:termAll] thread " << id << " after terminate: "
                      << (t ? std::to_string(static_cast<int>(t->status)) : std::string("gone"))
                      << (t ? " entry=0x" : "") << std::hex << (t ? t->entry : 0u) << std::dec << std::endl;
        }
    }

    // Movie audio. The movie player (Sony's ezmpeg sample) gets each PSS audio packet in pcmCallback and copies
    // its PCM into the audioDec ring (48 KiB), which audioDecSendToIOP drains into 989snd's IOP buffer as it
    // plays. With libmpeg the demuxer stops when the ring is full; the HLE's stream callbacks have no back-pressure,
    // and pcmCallback copied what fit and dropped the rest of the packet. A drop shifted the 16-bit PCM by an odd
    // byte count from then on: full-scale noise through the logo and intro movies, then a buzz when the IOP looped
    // its last buffers. Here the packets go through a host queue that feeds the ring with the game's own
    // bookkeeping (audioDecBeginPut/EndPut) as space frees, before every audioDecSendToIOP: no byte is lost.
    constexpr uint32_t kPcmCallback = 0x001ED5F8u;
    constexpr uint32_t kAudioDecSendToIop = 0x00202090u;
    constexpr uint32_t kAudioDecCreate = 0x00201E18u;
    constexpr uint32_t kAudioDecReset = 0x00201E90u;
    constexpr uint32_t kAudioDec = 0x004C7B48u;       // the player's AudioDec
    constexpr uint32_t kAudioDecHeaderBytes = 0x28u;  // 'SShd' + 'SSbd' headers, kept apart from the ring
    constexpr uint32_t kReadBufSizeOffset = 0x50008u; // ReadBuf: data[0x50000], then its size
    std::deque<uint8_t> g_moviePcm;
    uint32_t g_pcmCalls = 0;
    uint32_t g_sendCalls = 0;
    bool g_traceMovie = false;

    uint32_t guestU32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
        return value;
    }

    void setGuestU32(uint8_t *rdram, uint32_t address, uint32_t value)
    {
        std::memcpy(rdram + (address & 0x1FFFFFFu), &value, sizeof(value));
    }

    // audioDecBeginPut / audioDecEndPut (0x201f00 / 0x201fa0), with the copy pcmCallback makes in between.
    void feedMoviePcm(uint8_t *rdram)
    {
        const uint32_t ad = kAudioDec;
        while (!g_moviePcm.empty())
        {
            const uint32_t state = guestU32(rdram, ad + 0x00u);
            const uint32_t headerFill = guestU32(rdram, ad + 0x2Cu);
            const uint32_t base = guestU32(rdram, ad + 0x30u);
            const uint32_t writePos = guestU32(rdram, ad + 0x34u);
            const uint32_t count = guestU32(rdram, ad + 0x38u);
            const uint32_t size = guestU32(rdram, ad + 0x3Cu);
            if (size == 0u)
                return;
            uint32_t area0 = 0, length0 = 0, area1 = 0, length1 = 0;
            if (state == 0u)
            {
                area0 = ad + 4u + headerFill;
                length0 = kAudioDecHeaderBytes - headerFill;
                area1 = base;
                length1 = size;
            }
            else
            {
                const uint32_t free = size - count;
                area0 = base + writePos;
                length0 = std::min(free, size - writePos);
                area1 = base;
                length1 = free - length0;
            }
            const uint32_t take =
                static_cast<uint32_t>(std::min<size_t>(g_moviePcm.size(), static_cast<size_t>(length0) + length1));
            if (take == 0u)
                return;
            for (uint32_t i = 0; i < take; ++i)
                rdram[(i < length0 ? area0 + i : area1 + (i - length0)) & 0x1FFFFFFu] = g_moviePcm[i];
            g_moviePcm.erase(g_moviePcm.begin(), g_moviePcm.begin() + take);

            uint32_t ringBytes = take;
            if (state == 0u)
            {
                const uint32_t header = std::min(kAudioDecHeaderBytes - headerFill, take);
                setGuestU32(rdram, ad + 0x2Cu, headerFill + header);
                if (headerFill + header >= kAudioDecHeaderBytes)
                    setGuestU32(rdram, ad + 0x00u, 1u);
                ringBytes -= header;
            }
            setGuestU32(rdram, ad + 0x34u, (writePos + ringBytes) % size);
            setGuestU32(rdram, ad + 0x38u, count + ringBytes);
            setGuestU32(rdram, ad + 0x40u, guestU32(rdram, ad + 0x40u) + ringBytes);
        }
    }

    // pcmCallback(mpeg, cbData, readBuf): the packet minus its 4-byte sub-header (ff a0 00 00), which may wrap
    // around the read buffer, goes to the queue.
    void pcmCallbackOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t cbData = getRegU32(ctx, 5);
        const uint32_t readBuf = getRegU32(ctx, 6);
        const uint32_t data = guestU32(rdram, cbData + 0x08u);
        const uint32_t length = guestU32(rdram, cbData + 0x0Cu);
        const uint32_t ringSize = guestU32(rdram, readBuf + kReadBufSizeOffset);
        if (length > 4u && ringSize != 0u)
        {
            const uint32_t ringEnd = readBuf + ringSize;
            uint32_t source = data + 4u;
            if (source >= ringEnd)
                source -= ringSize;
            const uint32_t bytes = length - 4u;
            const uint32_t first = std::min(bytes, ringEnd - source);
            for (uint32_t i = 0; i < bytes; ++i)
                g_moviePcm.push_back(rdram[(i < first ? source + i : readBuf + (i - first)) & 0x1FFFFFFu]);
            feedMoviePcm(rdram);
        }
        if (g_traceMovie && g_pcmCalls < 8u)
            std::cerr << "[tmb:pcm] call " << g_pcmCalls + 1u << " cbData=0x" << std::hex << cbData << std::dec
                      << " bytes " << length << " queued " << g_moviePcm.size() << std::endl;
        ++g_pcmCalls;
        setReturnS32(ctx, 1);
        ctx->pc = getRegU32(ctx, 31);
    }

    void audioDecSendToIopOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        feedMoviePcm(rdram);
        const uint32_t ad = getRegU32(ctx, 4);
        const auto rd = [&](uint32_t off) { return static_cast<int32_t>(guestU32(rdram, ad + off)); };
        const int32_t stateBefore = rd(0x0);
        audioDecSendToIOP__FP8AudioDec_0x202090(rdram, ctx, runtime);
        if (g_traceMovie && (g_sendCalls++ % 50u) == 0u)
        {
            std::cerr << "[tmb:audio] send#" << g_sendCalls << " state " << stateBefore << "->" << rd(0x0)
                      << " +0x30=" << rd(0x30) << " +0x34=" << rd(0x34) << " +0x38=" << rd(0x38)
                      << " +0x3c=" << rd(0x3c) << " +0x44=0x" << std::hex << rd(0x44) << std::dec
                      << " +0x48=" << rd(0x48) << " +0x54=" << rd(0x54) << " pcmCalls=" << g_pcmCalls
                      << " queued=" << g_moviePcm.size() << std::endl;
        }
    }

    // A new movie (or a reset one) starts from an empty queue.
    void audioDecCreateOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_moviePcm.clear();
        audioDecCreate__FP8AudioDecPUcii_0x201e18(rdram, ctx, runtime);
    }

    void audioDecResetOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_moviePcm.clear();
        audioDecReset__FP8AudioDec_0x201e90(rdram, ctx, runtime);
    }

    constexpr uint32_t kVblankHandler = 0x001C2898u;
    uint32_t g_vblankCalls = 0;
    bool g_gsLag = false;
    bool g_gsLagAb = false;
    bool g_gsLagExperimentOn = true; // TMB_AB=lag: off in the experiment's off stretches

    // The GS lag (fork, PS2Memory::setGsLag: the GS and VU1 may finish a frame while the game runs the next) is
    // off while a movie plays: the main thread then waits for the decoder in a loop that only rotates the
    // priority-1 ready queue (switchThread) at priority 0, and running ahead it gets there before the decode
    // threads (priority 1) have queued their last pictures, which then never run. The movie flag is `displaying`
    // (gp-0x340C). The vblank handler, which only runs while movies play, turns the lag off; hier (each frame of
    // 3D rendering) turns it back on when no movie plays. TMB_GS_LAG_AB=1 switches it on and off every 100
    // vblanks instead (measurements).
    constexpr uint32_t kMovieDisplaying = 0x00506FF0u - 0x340Cu;
    constexpr uint32_t kHier = 0x001D40B0u;

    void updateGsLag(uint8_t *rdram, PS2Runtime *runtime)
    {
        if (!g_gsLag)
            return;
        bool lag = *reinterpret_cast<const int32_t *>(rdram + kMovieDisplaying) == 0 && g_gsLagExperimentOn;
        if (g_gsLagAb)
            lag = lag && (runtime->eeScheduler().currentVSyncTick() / 100u) % 2u == 0u;
        runtime->memory().setGsLag(lag);
    }

    double g_hierMs = 0, g_updateMs = 0;
    bool g_traceFrames = false;

    // TMB_GS_LAG_AB: per mode (lag on, off), every 500 frames, the frames, vblanks and slow frames (two vblanks or
    // more), and the input latency: vblanks from the TheGame::Update whose state a frame shows to the first vblank
    // after VIF1 finished drawing it (a callback queued behind the frame's packets), the earliest it can be shown.
    struct LagAbMode
    {
        uint64_t frames = 0, vblanks = 0, slow = 0;
        uint64_t drawn = 0, latencyVblanks = 0, latencyOver2 = 0;
    };
    std::mutex g_lagAbMutex;
    LagAbMode g_lagAbModes[2];
    uint64_t g_lastUpdateTick = 0; // vblank count at the last TheGame::Update

    // TMB_RAIN_LIFT_AB=1: the lifted rain (runtime/vu1/tmb_vu1_rain.cpp) off and on every 100 vblanks; per mode,
    // every 1000 frames: fps, slow frames (two vblanks or more), the VIF1 thread's CPU per frame, the strips lifted.
    bool g_rainAb = false;
    struct RainAbMode
    {
        uint64_t frames = 0, vblanks = 0, slow = 0, strips = 0;
        double vif1Ms = 0;
    };
    RainAbMode g_rainAbModes[2]; // lifted on, off

    void noteRainAbFrame(PS2Runtime *runtime, uint64_t vblanks)
    {
        if (!g_rainAb || vblanks == 0u || vblanks > 10u)
            return;
        static int64_t s_vif1Ns = -1;
        static uint64_t s_strips = 0, s_total = 0;
        const auto vif1Ns = []
        {
            for (const tmb::host::ThreadCpu &thread : tmb::host::threadCpuTimes())
                if (thread.name == "vif1")
                    return thread.cpuNs;
            return int64_t{0};
        };
        const int current = tmb::g_rainLiftOff.load(std::memory_order_relaxed) ? 1 : 0;
        RainAbMode &mode = g_rainAbModes[current];
        ++mode.frames;
        mode.vblanks += vblanks;
        mode.slow += vblanks >= 2u ? 1u : 0u;
        const int next = (runtime->eeScheduler().currentVSyncTick() / 100u) % 2u == 0u ? 0 : 1;
        if (next != current || s_vif1Ns < 0)
        {
            const int64_t now = vif1Ns();
            const uint64_t strips = tmb::g_rainLiftedStrips.load(std::memory_order_relaxed);
            if (s_vif1Ns >= 0)
            {
                mode.vif1Ms += static_cast<double>(now - s_vif1Ns) / 1e6;
                mode.strips += strips - s_strips;
            }
            s_vif1Ns = now;
            s_strips = strips;
            tmb::g_rainLiftOff.store(next == 1, std::memory_order_relaxed);
        }
        if (++s_total % 1000u != 0u)
            return;
        const auto describe = [](const RainAbMode &m)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "%llu frames %.1f fps %.1f%% slow, vif1 %.2f ms/frame, %llu strips lifted",
                          static_cast<unsigned long long>(m.frames), m.vblanks ? 50.0 * m.frames / m.vblanks : 0.0,
                          m.frames ? 100.0 * m.slow / m.frames : 0.0, m.frames ? m.vif1Ms / m.frames : 0.0,
                          static_cast<unsigned long long>(m.strips));
            return std::string(text);
        };
        std::fprintf(stderr, "[tmb:rainab] last 1000 frames, lifted: %s | compiled: %s\n",
                     describe(g_rainAbModes[0]).c_str(), describe(g_rainAbModes[1]).c_str());
        g_rainAbModes[0] = g_rainAbModes[1] = RainAbMode{};
    }

    // TMB_VBLANK_HOLD=<ms>: in the match, the vblank waits (fork, EeScheduler::setVBlankHold) while a frame is still
    // being processed, from TheGame::Update to the end of its real-time processing, at most <ms>: a frame that
    // overruns its field by a few ms then takes those ms instead of a second field (40 ms). The game counts one field
    // for it, so it runs that much slower in that frame instead of stuttering.
    uint32_t g_theGame = 0; // `this` of TheGame::Update
    bool g_vblankHoldNow = false;
    bool g_vblankHoldConfigured = false;

    // TMB_AB=<experiment>: an in-run A/B, the experiment on and off every 100 vblanks (the same stretches of play for
    // both); per mode, every 1000 frames: frames per second of real time, frames over 25 ms, and the game's speed
    // (fields of 20 ms against real time). Experiments: hold (the vblank hold; also TMB_VBLANK_HOLD_AB=1), slice
    // (equal-priority EE time slicing off, as the real kernel; also TMB_SLICE_AB=1), lag (the GS lag on), holdlimit
    // (the hold's limit at 12 ms against 6), fill (the GS drawing-time model at 8 pixels a GS cycle against 4).
    enum class AbExperiment
    {
        None,
        Hold,
        Slice,
        Lag,
        HoldLimit,
        Fill
    };
    AbExperiment g_ab = AbExperiment::None;
    bool g_abOnNow = false; // the experiment is on in this frame
    struct AbMode
    {
        uint64_t frames = 0, over25 = 0, fields = 0;
        double ms = 0;
    };
    AbMode g_abModes[2]; // off, on

    const char *abExperimentName(AbExperiment experiment)
    {
        switch (experiment)
        {
        case AbExperiment::Hold: return "vblank hold";
        case AbExperiment::Slice: return "equal-priority slicing off";
        case AbExperiment::Lag: return "GS lag";
        case AbExperiment::HoldLimit: return "hold limit 12 ms (against 6)";
        case AbExperiment::Fill: return "GS fill 8 px/cycle (against 4)";
        default: return "";
        }
    }

    void applyAbExperiment(PS2Runtime *runtime, bool on)
    {
        g_abOnNow = on;
        g_vblankHoldNow = g_vblankHoldConfigured;
        switch (g_ab)
        {
        case AbExperiment::Hold:
            g_vblankHoldNow = on;
            break;
        case AbExperiment::Slice:
            runtime->eeScheduler().setEqualPrioritySlicing(!on);
            break;
        case AbExperiment::Lag:
            g_gsLagExperimentOn = on;
            break;
        case AbExperiment::HoldLimit:
            runtime->eeScheduler().setVBlankHoldLimit(std::chrono::microseconds(on ? 12000 : 6000));
            break;
        case AbExperiment::Fill: // ffop/gs-fill-rate's default against main's, in the GS drawing-time model
            runtime->gsForPresentation().setDrawRates(GsDrawEstimate::Rates{on ? 256u : 512u, 32u});
            break;
        default:
            break;
        }
    }

    // At the end of each frame's real-time processing: the hold ends, the A/B counts the frame and picks the next mode.
    void noteVblankHoldFrame(PS2Runtime *runtime)
    {
        runtime->eeScheduler().setVBlankHold(false);
        if (g_ab == AbExperiment::None)
        {
            g_vblankHoldNow = g_vblankHoldConfigured;
            return;
        }
        static std::chrono::steady_clock::time_point s_last{};
        static uint64_t s_lastTick = 0, s_total = 0;
        const auto now = std::chrono::steady_clock::now();
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        if (s_last != std::chrono::steady_clock::time_point{} && g_lastUpdateTick > s_lastTick)
        {
            const double ms = std::chrono::duration<double, std::milli>(now - s_last).count();
            AbMode &mode = g_abModes[g_abOnNow ? 1 : 0];
            if (ms < 200.0)
            {
                ++mode.frames;
                mode.ms += ms;
                mode.over25 += ms > 25.0 ? 1u : 0u;
                mode.fields += tick - s_lastTick;
            }
            if (++s_total % 1000u == 0u)
            {
                const auto describe = [](const AbMode &m)
                {
                    char text[160];
                    std::snprintf(text, sizeof(text), "%llu frames %.1f fps %.1f%% over 25 ms, game speed %.2f",
                                  static_cast<unsigned long long>(m.frames), m.ms > 0 ? 1000.0 * m.frames / m.ms : 0.0,
                                  m.frames ? 100.0 * m.over25 / m.frames : 0.0, m.ms > 0 ? 20.0 * m.fields / m.ms : 0.0);
                    return std::string(text);
                };
                std::fprintf(stderr, "[tmb:ab] last 1000 frames, %s on: %s | off: %s | vblanks held %llu\n",
                             abExperimentName(g_ab), describe(g_abModes[1]).c_str(), describe(g_abModes[0]).c_str(),
                             static_cast<unsigned long long>(runtime->eeScheduler().vblanksHeld()));
                g_abModes[0] = g_abModes[1] = AbMode{};
            }
        }
        s_last = now;
        s_lastTick = tick;
        applyAbExperiment(runtime, (tick / 100u) % 2u == 1u);
    }

    void noteLagAbFrame(PS2Runtime *runtime, uint64_t vblanks)
    {
        if (!g_gsLagAb || vblanks == 0u || vblanks > 10u)
            return;
        static uint64_t s_total = 0;
        std::lock_guard<std::mutex> lock(g_lagAbMutex);
        LagAbMode &mode = g_lagAbModes[runtime->memory().gsLag() ? 0 : 1];
        ++mode.frames;
        mode.vblanks += vblanks;
        mode.slow += vblanks >= 2u ? 1u : 0u;
        if (++s_total % 500u != 0u)
            return;
        const auto describe = [](const LagAbMode &m)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "%llu frames %.1f fps %.1f%% slow, latency %.2f vblanks (%.1f%% over 2)",
                          static_cast<unsigned long long>(m.frames), m.vblanks ? 50.0 * m.frames / m.vblanks : 0.0,
                          m.frames ? 100.0 * m.slow / m.frames : 0.0,
                          m.drawn ? static_cast<double>(m.latencyVblanks) / m.drawn : 0.0,
                          m.drawn ? 100.0 * m.latencyOver2 / m.drawn : 0.0);
            return std::string(text);
        };
        std::fprintf(stderr, "[tmb:lagab] last 500 frames, lag on: %s | off: %s\n", describe(g_lagAbModes[0]).c_str(),
                     describe(g_lagAbModes[1]).c_str());
        g_lagAbModes[0] = g_lagAbModes[1] = LagAbMode{};
    }

    // TMB_TRACE_IDLE=1: what the EE waits on when every guest thread is blocked (EeScheduler::setIdleObserver),
    // per frame, added up separately for slow frames (two vblanks or more) and fast ones; every 1500 frames the
    // largest per class, in ms per frame: "<blocked threads> <- <what woke the EE>".
    bool g_traceIdle = false;
    std::vector<EeScheduler::IdleRecord> g_frameIdle;

    void noteIdleFrame(uint64_t vblanks)
    {
        if (!g_traceIdle)
            return;
        struct Totals
        {
            uint64_t frames = 0;
            std::map<std::string, double> ms;
        };
        static Totals s_totals[2];
        static uint64_t s_frames = 0;
        if (vblanks != 0u && vblanks <= 10u)
        {
            Totals &totals = s_totals[vblanks >= 2u ? 1 : 0];
            ++totals.frames;
            for (const EeScheduler::IdleRecord &record : g_frameIdle)
                totals.ms[record.blocked + " <- " + record.wokenBy] += static_cast<double>(record.nanoseconds) / 1e6;
        }
        g_frameIdle.clear();
        if (++s_frames % 1500u != 0u)
            return;
        for (int kind = 0; kind < 2; ++kind)
        {
            const Totals &totals = s_totals[kind];
            if (totals.frames == 0u)
                continue;
            std::vector<std::pair<double, std::string>> sorted;
            double all = 0.0;
            for (const auto &[key, ms] : totals.ms)
            {
                sorted.push_back({ms, key});
                all += ms;
            }
            std::sort(sorted.rbegin(), sorted.rend());
            std::fprintf(stderr, "[tmb:idle] %s frames: %llu, EE idle %.2f ms per frame\n", kind ? "slow" : "fast",
                         static_cast<unsigned long long>(totals.frames), all / totals.frames);
            for (size_t i = 0; i < sorted.size() && i < 12u; ++i)
                std::fprintf(stderr, "[tmb:idle]   %6.2f ms  %s\n", sorted[i].first / totals.frames, sorted[i].second.c_str());
        }
    }

    void hierOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        updateGsLag(rdram, runtime);
        const auto start = std::chrono::steady_clock::now();
        timelineEvent('H');
        hier__Fi_0x1d40b0(rdram, ctx, runtime);
        timelineEvent('h');
        // hier draws the state of the TheGame::Update before it (the frame's own Update comes after).
        if (g_gsLagAb && g_lastUpdateTick != 0u && runtime->memory().vif1Threaded())
        {
            const uint64_t updateTick = g_lastUpdateTick;
            const int lag = runtime->memory().gsLag() ? 0 : 1;
            runtime->memory().queueVif1Callback([runtime, updateTick, lag]()
            {
                const uint64_t shown = runtime->eeScheduler().currentVSyncTick() + 1u;
                std::lock_guard<std::mutex> lock(g_lagAbMutex);
                LagAbMode &mode = g_lagAbModes[lag];
                ++mode.drawn;
                mode.latencyVblanks += shown - updateTick;
                mode.latencyOver2 += shown - updateTick > 2u ? 1u : 0u;
            });
        }
        if (g_traceFrames)
            g_hierMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void vblankHandlerOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const auto rd = [&](uint32_t addr) { return *reinterpret_cast<const int32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const int32_t displaying = rd(gp - 0x340Cu);
        timelineEvent('B');
        vblankHandler__Fi_0x1c2898(rdram, ctx, runtime);
        updateGsLag(rdram, runtime);
        if (!g_traceMovie)
            return;
        static uint32_t s_dispCalls = 0;
        if (std::getenv("TMB_TRACE_DISPLAY") && (s_dispCalls++ % 150u) == 0u)
        {
            PS2Memory &m = runtime->memory();
            std::cerr << std::hex << "[tmb:disp] pmode=0x" << m.read64(0x12000000u) << " smode2=0x" << m.read64(0x12000020u)
                      << " dispfb1=0x" << m.read64(0x12000070u) << " display1=0x" << m.read64(0x12000080u)
                      << " dispfb2=0x" << m.read64(0x12000090u) << " display2=0x" << m.read64(0x120000A0u)
                      << " bg=0x" << m.read64(0x120000E0u) << std::dec << std::endl;
        }
        if (displaying && (g_vblankCalls++ % 100u) == 0u)
        {
            std::cerr << "[tmb:vblank] call#" << g_vblankCalls << " displaying=" << displaying
                      << " pathBusy=" << rd(gp - 0x3400u) << " ret=" << static_cast<int32_t>(getRegU32(ctx, 2))
                      << " voBuf(+8,+0x10,+0xc)=" << rd(0x4C7A18u + 8u) << "," << rd(0x4C7A18u + 0x10u) << ","
                      << rd(0x4C7A18u + 0xCu) << " | " << ps2_stubs::mpegDebugSummary() << std::endl;
        }
    }

    // TMB_TRACE_FONT=1: dump the font packet state on each fontDmaFontData (bring-up aid).
    constexpr uint32_t kFontDmaFontData = 0x001C4FE0u;
    uint32_t g_fontCalls = 0;

    void fontDmaFontDataTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const auto rd32 = [&](uint32_t addr) { return *reinterpret_cast<const int32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const auto rd64 = [&](uint32_t addr) { return *reinterpret_cast<const uint64_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const int32_t state = rd32(gp - 0x2D28u), count = rd32(gp - 0x2D2Cu), base = rd32(gp - 0x2D20u);
        const int32_t tagIdx = rd32(gp - 0x2D24u), prims = static_cast<int32_t>(rd32(gp - 0x2D30u) & 0xFFFF);
        fontDmaFontData__Fv_0x1c4fe0(rdram, ctx, runtime);
        const uint32_t n = g_fontCalls++;
        if (n < 6u || (n % 200u) == 0u || (state == 2 && n < 400u && (n % 20u) == 0u))
        {
            constexpr uint32_t kBuf = 0x0050CB60u;
            std::cerr << std::hex << "[tmb:font] call#" << std::dec << n << " state=" << state << " count=" << count
                      << " base=" << base << " tagIdx=" << tagIdx << " prims=" << prims << std::hex
                      << " | tag@idx=0x" << rd64(kBuf + 16u * static_cast<uint32_t>(tagIdx)) << "_" << rd64(kBuf + 16u * static_cast<uint32_t>(tagIdx) + 8u)
                      << " q0=0x" << rd64(kBuf) << "_" << rd64(kBuf + 8u) << " after: count=" << std::dec << rd32(gp - 0x2D2Cu)
                      << " tagIdx=" << rd32(gp - 0x2D24u) << " state=" << rd32(gp - 0x2D28u) << std::endl;
        }
    }

    // TMB_TRACE_THREADS=<seconds>: print every guest thread and semaphore that often (hang hunting).
    void startThreadTrace(PS2Runtime &runtime, double periodSeconds)
    {
        std::thread([&runtime, periodSeconds]()
                    {
                        static const char *const kStatus[] = {"run", "ready", "wait", "wait+susp", "susp", "dormant"};
                        static const char *const kWait[] = {"-", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
                        for (uint32_t pass = 0;; ++pass)
                        {
                            std::this_thread::sleep_for(std::chrono::duration<double>(periodSeconds));
                            const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
                            std::cerr << "[tmb:threads] pass " << pass << " running=" << snap.runningThreadId
                                      << " | mpeg: " << ps2_stubs::mpegDebugSummary() << std::endl;
                            for (const EeThreadSnapshot &t : snap.threads)
                            {
                                const auto st = static_cast<size_t>(t.status);
                                const auto wr = static_cast<size_t>(t.waitReason);
                                std::cerr << std::hex << "  th" << std::dec << t.id << " " << (st < 6 ? kStatus[st] : "?")
                                          << " wait=" << (wr < 7 ? kWait[wr] : "?") << ":" << t.waitId
                                          << " prio=" << t.currentPriority << std::hex << " pc=0x" << t.pc
                                          << " ra=0x" << t.ra << " sp=0x" << t.sp << " entry=0x" << t.entry << std::dec
                                          << " wakeups=" << t.wakeupCount << " depth=" << t.invocationDepth << std::endl;
                            }
                            {
                                const auto &g = runtime.memory().gs();
                                std::cerr << std::hex << "  [gs] pmode=0x" << g.pmode << " smode2=0x" << g.smode2
                                          << " dispfb1=0x" << g.dispfb1 << " display1=0x" << g.display1
                                          << " dispfb2=0x" << g.dispfb2 << " display2=0x" << g.display2
                                          << std::dec << std::endl;
                            }
                            if (std::getenv("TMB_TRACE_IOP"))
                            {
                                const auto iop = runtime.iopDebugSnapshot();
                                for (const std::string &line : iop.diagnostics)
                                {
                                    std::cerr << "  [iop] " << line << std::endl;
                                }
                            }
                            for (const EeSemaphoreSnapshot &sem : snap.semaphores)
                            {
                                if (sem.waiters != 0u)
                                {
                                    std::cerr << "  sema" << sem.id << " count=" << sem.count << "/" << sem.maxCount
                                              << " waiters=" << sem.waiters << std::endl;
                                }
                            }
                        } })
            .detach();
    }

    // End of a movie. Every PSS ends with 4 bytes past its last sector (the program end code); the
    // player treats the stream as complete with < 5 bytes left and never reads them. It then calls
    // videoDecFlush, which appends a sequence_end_code to the IPU ring so the decoder finishes. With
    // libmpeg HLE'd, tell the HLE the input is complete once that flush succeeds.
    constexpr uint32_t kVideoDecFlush = 0x001FF9E0u;

    void videoDecFlushOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t videoDec = getRegU32(ctx, 4);
        videoDecFlush__FP8VideoDec_0x1ff9e0(rdram, ctx, runtime);
        if (getRegU32(ctx, 2) != 0u)
        {
            ps2_stubs::endMpegPlaybackInput(runtime, videoDec);
        }
    }

    // TMB_TRACE_SND=1: log every 989snd command sent with snd_SendIOPCommandAndWait (bring-up aid).
    constexpr uint32_t kSndSendIopCommandAndWait = 0x001BCE28u;

    void sndSendIopCommandAndWaitTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0;
        const uint32_t command = getRegU32(ctx, 4);
        const int32_t count = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t args = getRegU32(ctx, 6);
        const uint32_t n = s_calls++;
        std::cerr << std::hex << "[tmb:snd] #" << std::dec << n << " cmd=0x" << std::hex << command << " argc=" << std::dec << count;
        for (int32_t i = 0; i < count && i < 6; ++i)
        {
            std::cerr << std::hex << " 0x" << *reinterpret_cast<const uint32_t *>(rdram + ((args + 4u * static_cast<uint32_t>(i)) & 0x1FFFFFFu));
        }
        std::cerr << std::dec << std::endl;
        snd_SendIOPCommandAndWait_0x1bce28(rdram, ctx, runtime);
        std::cerr << "[tmb:snd] #" << n << " -> 0x" << std::hex << getRegU32(ctx, 2) << std::dec << std::endl;
    }

    // TMB_TRACE_HIER=1: count VIF1 DMA completion handler calls of the hier renderer (bring-up aid).
    constexpr uint32_t kHierDmaHandler = 0x001D63A0u;
    std::atomic<uint32_t> g_hierDmaCalls{0};

    void hierDmaHandlerTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t channel = getRegU32(ctx, 4);
        const auto rd = [&](uint32_t addr) { return *reinterpret_cast<const uint32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const uint32_t before0 = rd(0x00503CF8u), before1 = rd(0x00503CFCu);
        hierDmaHandler__Fi_0x1d63a0(rdram, ctx, runtime);
        const uint32_t n = g_hierDmaCalls.fetch_add(1);
        if (n < 20u || (n % 500u) == 0u)
            std::cerr << "[tmb:hier] dma handler #" << n << " ch=" << channel << " flags " << before0 << "," << before1
                      << " -> " << rd(0x00503CF8u) << "," << rd(0x00503CFCu) << std::endl;
    }

    // The game measures each frame with T0 (BUSCLK/256, 576 ticks per ms) into a global that timerGetFrameTime turns
    // into milliseconds, and moves weapons and debris over it without a limit. Built for 50 fps (20 ms), it blows
    // up at the 80-100 ms frames of a slow host. Capped at 40 ms (TMB_MAX_FRAME_MS), they move in slow motion
    // instead when the host cannot keep up. (The cars use the vblank fields: theGameUpdateOverride below.)
    constexpr uint32_t kTimerEndOfRealTimeProcessing = 0x001F7A00u;
    constexpr int32_t kFrameTicksGpOffset = -0x2BDC; // timerGetFrameTime's tick count (gp-relative)
    int32_t g_maxFrameTicks = 40 * 576;

    // TMB_TRACE_FRAMES=1: every 100 frames, the host time from the end of one frame's real-time processing to the
    // next TheGame::Update (W: vsync wait, rendering kick, pad) and from there to the end of the processing (B), and how
    // many of those frames took over 25 ms in all.
    std::chrono::steady_clock::time_point g_frameEnd, g_frameUpdate;

    // CPU time of each host thread per frame since the previous call, by thread name.
    std::string threadCpuPerFrame(uint32_t frames)
    {
        static std::unordered_map<uint64_t, int64_t> s_previous;
        std::map<std::string, double> perName;
        for (const tmb::host::ThreadCpu &thread : tmb::host::threadCpuTimes())
        {
            const auto previous = s_previous.find(thread.id);
            if (previous != s_previous.end())
                perName[thread.name.empty() ? "?" : thread.name] += static_cast<double>(thread.cpuNs - previous->second) / 1e6 / frames;
            s_previous[thread.id] = thread.cpuNs;
        }
        std::string out;
        char buffer[64];
        for (const auto &[name, ms] : perName)
            if (ms >= 0.5)
            {
                std::snprintf(buffer, sizeof(buffer), " %s %.1f", name.c_str(), ms);
                out += buffer;
            }
        return out;
    }

    PS2Runtime *g_runtime = nullptr;

    void traceFrameEnd()
    {
        static double sumW = 0, sumB = 0, maxW = 0, maxB = 0, sumH = 0, sumU = 0;
        static uint32_t frames = 0, over25 = 0;
        const auto now = std::chrono::steady_clock::now();
        if (g_frameEnd.time_since_epoch().count() != 0 && g_frameUpdate > g_frameEnd)
        {
            const double w = std::chrono::duration<double, std::milli>(g_frameUpdate - g_frameEnd).count();
            const double b = std::chrono::duration<double, std::milli>(now - g_frameUpdate).count();
            sumW += w; sumB += b; maxW = std::max(maxW, w); maxB = std::max(maxB, b);
            sumH += g_hierMs; sumU += g_updateMs;
            over25 += w + b > 25.0 ? 1u : 0u;
            if (++frames == 100u)
            {
                static uint64_t s_idle = 0, s_vif1 = 0;
                const uint64_t idle = g_runtime->eeScheduler().idleWaitNanoseconds();
                const uint64_t vif1 = g_runtime->memory().vif1WaitNanoseconds();
                std::fprintf(stderr, "[tmb:frames] W avg %.1f max %.1f ms, B avg %.1f max %.1f ms, hier %.1f, update %.1f, "
                             "EE idle %.1f, waiting VIF1/GS %.1f, over 25 ms %u | cpu ms/frame:%s\n",
                             sumW / frames, maxW, sumB / frames, maxB, sumH / frames, sumU / frames,
                             (idle - s_idle) / 1e6 / frames, (vif1 - s_vif1) / 1e6 / frames, over25, threadCpuPerFrame(frames).c_str());
                s_idle = idle;
                s_vif1 = vif1;
                sumW = sumB = maxW = maxB = sumH = sumU = 0;
                frames = over25 = 0;
            }
        }
        g_frameEnd = now;
        g_hierMs = g_updateMs = 0;
    }

    void timerEndOfRealTimeProcessingOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_runtime = runtime;
        timelineEvent('E');
        runtime->noteGameFrame(); // the fps overlay
        noteVblankHoldFrame(runtime); // TMB_VBLANK_HOLD: the frame's processing is done
        // TMB_PLAYER_HEALTH_CAP=<x> (testing): the player car's health (vehicle 0x259c70, float at +1012; +1008 is
        // what gameCheckPlayerHealth sets it to) held at most x, so a few hits end a life: the game-over
        // screens within minutes of play.
        static const float s_healthCap = []
        {
            const char *value = std::getenv("TMB_PLAYER_HEALTH_CAP");
            return value && *value ? static_cast<float>(std::atof(value)) : -1.0f;
        }();
        // TMB_ENEMY_HEALTH_CAP=<x> (testing): every vehicle but the player's held at most x health, so a scripted run
        // can kill its way through story mode. The vehicles are TheGame's array (TheGame::GetVehicleFromName, 0x130bc8):
        // vehicle i at TheGame + i * 0x2D00, their count at TheGame + 0x1B3DB4; TheGame is `this` of TheGame::Update.
        static const float s_enemyCap = []
        {
            const char *value = std::getenv("TMB_ENEMY_HEALTH_CAP");
            return value && *value ? static_cast<float>(std::atof(value)) : -1.0f;
        }();
        if (s_enemyCap >= 0.0f && g_theGame != 0u)
        {
            int32_t count = 0;
            std::memcpy(&count, rdram + ((g_theGame + 0x001B3DB4u) & 0x1FFFFFFu), 4);
            static uint32_t s_enemyFrames = 0;
            const bool log = ++s_enemyFrames % 500u == 0u;
            std::string line;
            for (int32_t index = 0; index < std::min(count, 64); ++index)
            {
                const uint32_t vehicle = g_theGame + static_cast<uint32_t>(index) * 0x2D00u;
                float health = 0.0f;
                std::memcpy(&health, rdram + ((vehicle + 1012u) & 0x1FFFFFFu), 4);
                if (log)
                {
                    char text[48];
                    std::snprintf(text, sizeof(text), " %x:%.0f", vehicle, health);
                    line += text;
                }
                if (vehicle != 0x00259C70u && health > s_enemyCap)
                {
                    health = s_enemyCap;
                    std::memcpy(rdram + ((vehicle + 1012u) & 0x1FFFFFFu), &health, 4);
                }
            }
            if (log)
                std::fprintf(stderr, "[tmb:enemies] TheGame %x, %d vehicles:%s\n", g_theGame, count, line.c_str());
        }
        // TMB_PLAYER_GOD=1 (testing): the player car's health (float at +1012) back to the highest value seen every
        // frame, so it never dies (story-mode walkthroughs). Health only falls during a life; a new car starts full.
        static const bool s_playerGod = std::getenv("TMB_PLAYER_GOD") != nullptr;
        if (s_playerGod)
        {
            static float s_best = 0.0f;
            float health = 0.0f;
            std::memcpy(&health, rdram + 0x00259C70u + 1012u, 4);
            if (health > s_best && health < 10000.0f)
                s_best = health;
            else if (health < s_best && health > -10000.0f)
                std::memcpy(rdram + 0x00259C70u + 1012u, &s_best, 4);
        }
        if (s_healthCap >= 0.0f)
        {
            float health = 0.0f, maximum = 0.0f;
            std::memcpy(&health, rdram + 0x00259C70u + 1012u, 4);
            std::memcpy(&maximum, rdram + 0x00259C70u + 1008u, 4);
            static uint32_t s_frames = 0;
            if (++s_frames % 250u == 0u)
                std::fprintf(stderr, "[tmb:health] %.1f of %.1f\n", health, maximum);
            if (health > s_healthCap)
            {
                health = s_healthCap;
                std::memcpy(rdram + 0x00259C70u + 1012u, &health, 4);
            }
        }
        // PS2X_PROFILE_GATED=1: the profiler samples while frames take two vblanks or more (busy scenes; they come
        // in stretches, so the frame after a slow one is mostly slow too), or, with TMB_PROFILE_FAST_FRAMES=1,
        // while they take one: a profile of each kind from the same build.
        {
            static const bool s_fastFrames = []
            {
                const char *value = std::getenv("TMB_PROFILE_FAST_FRAMES");
                return value && *value == '1';
            }();
            static uint64_t s_lastTick = 0;
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            const bool slow = tick - s_lastTick >= 2u;
            ps2x::profiling::setSampleGate(s_lastTick != 0 && slow != s_fastFrames);
            if (s_lastTick != 0)
            {
                noteLagAbFrame(runtime, tick - s_lastTick);
                noteRainAbFrame(runtime, tick - s_lastTick);
                noteIdleFrame(tick - s_lastTick);
            }
            s_lastTick = tick;
        }
        if (g_traceFrames)
            traceFrameEnd();
        const uint32_t gp = getRegU32(ctx, 28);
        tmb::native::onFrameEnd(); // TMB_NATIVE: publish the frame's native draws
        timerEndOfRealTimeProcessing__Fv_0x1f7a00(rdram, ctx, runtime);
        int32_t *frameTicks = reinterpret_cast<int32_t *>(rdram + ((gp + kFrameTicksGpOffset) & 0x1FFFFFFu));
        if (*frameTicks > g_maxFrameTicks)
            *frameTicks = g_maxFrameTicks;
    }

    // TheGame::Update moves each car by its velocity times the vblank fields since the previous frame
    // (timerGetFieldsLastFrame, copied to vehicle+0x20C8) and tests the collisions once, after the move. Built for
    // 50 fps (one field per frame, two when the PS2 is overloaded), at the two to four fields of a slow host each
    // step goes two to four times as far, and fast cars landing or hitting a slope go through the ground (bots fall
    // under the arena until the game teleports them back). Run it once per field instead, the fields, update rate
    // and frame time (weapons, debris) split over the steps, at most TMB_SUBSTEPS (default 4, 1 disables) steps.
    // Update ends in a tail call and may be preempted, so each step returns through the dispatcher to
    // kSubstepReturn (padding between two functions, never executed), which starts the next one.
    constexpr uint32_t kTheGameUpdate = 0x0012FBA8u;
    constexpr uint32_t kSubstepReturn = 0x0012FBA4u;
    constexpr int32_t kFieldsLastFrameGpOffset = -0x2BF0;
    constexpr int32_t kUpdateRateGpOffset = -0x2BEC;
    constexpr int32_t kFieldRateGpOffset = -0x2BD0; // fields per second (50)
    int32_t g_maxSubsteps = 4;

    struct Substeps
    {
        int32_t step = 0, steps = 0;
        int32_t totalFields = 0, totalRate = 0, totalTicks = 0, ticksDone = 0;
        uint32_t self = 0, ret = 0, gp = 0;
    } g_substeps;

    int32_t *gpGlobal(uint8_t *rdram, int32_t offset)
    {
        return reinterpret_cast<int32_t *>(rdram + ((g_substeps.gp + offset) & 0x1FFFFFFu));
    }

    void runSubstep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Substeps &s = g_substeps;
        const int32_t fields = s.step + 1 < s.steps ? 1 : s.totalFields - (s.steps - 1);
        *gpGlobal(rdram, kFieldsLastFrameGpOffset) = fields;
        *gpGlobal(rdram, kUpdateRateGpOffset) = std::max(1, *gpGlobal(rdram, kFieldRateGpOffset) / fields);
        const int32_t ticks = static_cast<int32_t>(static_cast<int64_t>(s.totalTicks) * (s.step + 1) / s.steps) - s.ticksDone;
        s.ticksDone += ticks;
        *gpGlobal(rdram, kFrameTicksGpOffset) = ticks;
        SET_GPR_U32(ctx, 4, s.self);
        SET_GPR_U32(ctx, 31, kSubstepReturn);
        ctx->pc = kTheGameUpdate;
        Update__7TheGame_0x12fba8(rdram, ctx, runtime);
    }

    void substepReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Substeps &s = g_substeps;
        if (++s.step < s.steps)
        {
            runSubstep(rdram, ctx, runtime);
            return;
        }
        *gpGlobal(rdram, kFieldsLastFrameGpOffset) = s.totalFields;
        *gpGlobal(rdram, kUpdateRateGpOffset) = s.totalRate;
        *gpGlobal(rdram, kFrameTicksGpOffset) = s.totalTicks;
        s.steps = 0;
        if (g_traceFrames)
            g_updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_frameUpdate).count();
        SET_GPR_U32(ctx, 31, s.ret);
        ctx->pc = s.ret;
    }

    // TMB_TRACE_TIMELINE=<file>: 4 s of the match (from its 500th TheGame::Update; TMB_TRACE_TIMELINE_START and
    // TMB_TRACE_TIMELINE_SECONDS move and stretch the window) as a timeline, for
    // tools/timeline.py: the game's events (vblank handler, Update, hier, sceGsSyncPath / sceGsSyncV enter and exit,
    // end of the real-time processing) and, every 0.2 ms, the CPU time of the game, vif1 and gs threads.
    struct Timeline
    {
        struct Event
        {
            int64_t ns;
            char type;
            uint32_t arg;
        };
        struct Sample
        {
            int64_t ns;
            int64_t cpu[3];
        };
        std::string path;
        std::atomic<int> state{0}; // 0 waiting, 1 recording, 2 done
        uint32_t updates = 0;
        uint32_t startUpdate = 500u;
        int64_t durationNs = 4000000000ll;
        int64_t startNs = 0;
        std::mutex mutex;
        std::vector<Event> events;
        std::vector<Sample> samples;
        std::thread sampler;
    } g_timeline;

    int64_t timelineNow()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void timelineEvent(char type, uint32_t arg)
    {
        if (g_timeline.state.load(std::memory_order_relaxed) != 1)
            return;
        std::lock_guard<std::mutex> lock(g_timeline.mutex);
        g_timeline.events.push_back({timelineNow(), type, arg});
    }

    void timelineDump()
    {
        FILE *out = std::fopen(g_timeline.path.c_str(), "w");
        if (!out)
            return;
        std::lock_guard<std::mutex> lock(g_timeline.mutex);
        for (const auto &e : g_timeline.events)
            std::fprintf(out, "e %lld %c %x\n", static_cast<long long>(e.ns - g_timeline.startNs), e.type, e.arg);
        for (const auto &s : g_timeline.samples)
            std::fprintf(out, "s %lld %lld %lld %lld\n", static_cast<long long>(s.ns - g_timeline.startNs),
                         static_cast<long long>(s.cpu[0]), static_cast<long long>(s.cpu[1]), static_cast<long long>(s.cpu[2]));
        std::fclose(out);
        std::fprintf(stderr, "[tmb:timeline] %zu events, %zu samples written to %s\n", g_timeline.events.size(),
                     g_timeline.samples.size(), g_timeline.path.c_str());
    }

    void timelineStart()
    {
        // The threads' CPU clocks.
        int64_t clocks[3];
        const char *names[3] = {"GameThread", "vif1", "GSFrontend"};
        for (int i = 0; i < 3; ++i)
            clocks[i] = tmb::host::findThreadCpuClock(names[i]);
        g_timeline.startNs = timelineNow();
        g_timeline.state.store(1);
        g_timeline.sampler = std::thread([clocks]()
                                         {
            // The vblank handler only runs during movies: mark the vblanks from the scheduler's tick instead.
            uint64_t lastVSyncTick = g_runtime ? g_runtime->eeScheduler().currentVSyncTick() : 0u;
            while (g_timeline.state.load() == 1)
            {
                if (g_runtime)
                {
                    const uint64_t tick = g_runtime->eeScheduler().currentVSyncTick();
                    if (tick != lastVSyncTick)
                    {
                        lastVSyncTick = tick;
                        std::lock_guard<std::mutex> lock(g_timeline.mutex);
                        g_timeline.events.push_back({timelineNow(), 'B', static_cast<uint32_t>(tick)});
                    }
                }
                Timeline::Sample sample{timelineNow(), {0, 0, 0}};
                for (int i = 0; i < 3; ++i)
                    sample.cpu[i] = tmb::host::readThreadCpuNs(clocks[i]);
                {
                    std::lock_guard<std::mutex> lock(g_timeline.mutex);
                    g_timeline.samples.push_back(sample);
                }
                if (sample.ns - g_timeline.startNs > g_timeline.durationNs)
                {
                    g_timeline.state.store(2);
                    timelineDump();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            } });
        g_timeline.sampler.detach();
    }

    void sceGsSyncPathTimeline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        timelineEvent('P', ra);
        sceGsSyncPath_0x204e28(rdram, ctx, runtime);
        timelineEvent('p', ra);
    }

    void sceGsSyncVTimeline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        timelineEvent('V', ra);
        sceGsSyncV_0x204d90(rdram, ctx, runtime);
        timelineEvent('v', ra);
    }

    // TMB_KILL_ENEMIES=<s> (testing): every <s> seconds of TheGame::Update, every opponent still alive takes a 10000
    // point hit through the game's own Vehicle::vehicleTakeHit (0x1ae070: a0 vehicle, a1 hit point, a2 damage, a3
    // type), so it dies as if shot and the kill counts ("kill 7 enemies to advance"). A scripted run then gets through
    // story levels without aiming. Called on a copy of the context at the top of the update, where the stack below sp
    // is free.
    void killEnemies(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint32_t s_interval = []
        {
            const char *value = std::getenv("TMB_KILL_ENEMIES");
            return value && *value ? static_cast<uint32_t>(std::atof(value) * 50.0) : 0u;
        }();
        static uint32_t s_updates = 0;
        if (s_interval == 0u || g_theGame == 0u || ++s_updates % s_interval != 0u)
            return;
        int32_t count = 0;
        std::memcpy(&count, rdram + ((g_theGame + 0x001B3DB4u) & 0x1FFFFFFu), 4);
        int killed = 0;
        for (int32_t index = 0; index < std::min(count, 64); ++index)
        {
            const uint32_t vehicle = g_theGame + static_cast<uint32_t>(index) * 0x2D00u;
            float health = 0.0f;
            std::memcpy(&health, rdram + ((vehicle + 1012u) & 0x1FFFFFFu), 4);
            if (vehicle == 0x00259C70u || !(health > 0.0f))
                continue;
            R5900Context call = *ctx;
            SET_GPR_U32(&call, 4, vehicle);
            SET_GPR_U32(&call, 5, vehicle); // the hit point: only placement for the effects
            SET_GPR_U32(&call, 6, 10000u);
            SET_GPR_U32(&call, 7, 0u);
            vehicleTakeHit__7VehicleP8_fvectorii_0x1ae070(rdram, &call, runtime);
            ++killed;
        }
        if (killed)
            std::fprintf(stderr, "[tmb:kill] %d of %d vehicles hit\n", killed, count);
    }

    // TMB_DUMP_RDRAM=<s> (native port work): after <s> seconds of TheGame::Update (50 updates a second), the 32 MB of
    // EE memory to build/rdram.bin, once, for offline decoding of the game's own data (texture table at 0x4D4930,
    // meshes): tools/tmb_textures.py.
    void dumpRdramOnce(uint8_t *rdram)
    {
        static const uint32_t s_at = []
        {
            const char *value = std::getenv("TMB_DUMP_RDRAM");
            return value && *value ? static_cast<uint32_t>(std::atof(value) * 50.0) : 0u;
        }();
        static uint32_t s_updates = 0;
        if (s_at == 0u || ++s_updates != s_at)
            return;
        if (FILE *out = std::fopen("build/rdram.bin", "wb"))
        {
            std::fwrite(rdram, 1, PS2_RAM_SIZE, out);
            std::fclose(out);
            std::fprintf(stderr, "[tmb:dump] EE memory written to build/rdram.bin\n");
        }
    }

    // TMB_NATIVE: hierFlushObjQ kicks the VIF1 chain of the frame's objects; read them for the native renderer first.
    constexpr uint32_t kHierFlushObjQ = 0x001D62C0u;

    void hierFlushObjQOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        tmb::native::onFlushObjQ(rdram);
        hierFlushObjQ__Fv_0x1d62c0(rdram, ctx, runtime);
    }

    void theGameUpdateOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_theGame = getRegU32(ctx, 4); // TMB_ENEMY_HEALTH_CAP, TMB_KILL_ENEMIES
        if (g_substeps.steps == 0)
        {
            killEnemies(rdram, ctx, runtime);
            dumpRdramOnce(rdram);
        }
        Substeps &s = g_substeps;
        if (!g_timeline.path.empty() && s.steps == 0)
        {
            if (g_timeline.state.load() == 0 && ++g_timeline.updates == g_timeline.startUpdate)
                timelineStart();
            timelineEvent('U', static_cast<uint32_t>(*gpGlobal(rdram, kFieldsLastFrameGpOffset)));
        }
        if (g_traceFrames && s.steps == 0)
            g_frameUpdate = std::chrono::steady_clock::now();
        if (s.steps == 0)
            g_lastUpdateTick = runtime->eeScheduler().currentVSyncTick();
        if (s.steps == 0 && g_vblankHoldNow)
            runtime->eeScheduler().setVBlankHold(true); // until the end of this frame's processing
        s.gp = getRegU32(ctx, 28);
        const int32_t fields = *gpGlobal(rdram, kFieldsLastFrameGpOffset);
        const int32_t steps = std::min(fields, g_maxSubsteps);
        if (steps <= 1 || s.steps != 0)
        {
            Update__7TheGame_0x12fba8(rdram, ctx, runtime);
            return;
        }
        s.step = 0;
        s.steps = steps;
        s.totalFields = fields;
        s.totalRate = *gpGlobal(rdram, kUpdateRateGpOffset);
        s.totalTicks = *gpGlobal(rdram, kFrameTicksGpOffset);
        s.ticksDone = 0;
        s.self = getRegU32(ctx, 4);
        s.ret = getRegU32(ctx, 31);
        runSubstep(rdram, ctx, runtime);
    }

    // TMB_TRACE_BLACKBOX=<file>: the player vehicle (0x259c70, 0x2200 bytes) at every collisCheckHdResults; when its
    // position goes past 50000 the last 64 frames are written to <file> (bring-up aid for the physics blow-ups).
    constexpr uint32_t kPlayerVehicle = 0x00259C70u;
    constexpr uint32_t kVehicleBytes = 0x2200u;
    constexpr uint32_t kBlackboxFrames = 64u;
    std::vector<uint8_t> g_blackbox;
    uint32_t g_blackboxNext = 0;
    bool g_blackboxDumped = false;
    const char *g_blackboxPath = nullptr;

    // TMB_TRACE_POS=<file>: every vehicle's position (+0x260) at each collisCheckHdResults, one line per call (vsync
    // tick, vehicle, x, y, z): falls through the ground show as the height dropping under the terrain's.
    FILE *g_posTrace = nullptr;

    void collisCheckHdResultsBlackbox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_posTrace)
        {
            const uint32_t vehicle = getRegU32(ctx, 4);
            float p[3];
            std::memcpy(p, rdram + ((vehicle + 0x260u) & 0x1FFFFFFu), sizeof(p));
            std::fprintf(g_posTrace, "%llu %x %.2f %.2f %.2f\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), vehicle, p[0], p[1],
                         p[2]);
        }
        if (g_blackboxPath && getRegU32(ctx, 4) == kPlayerVehicle && !g_blackboxDumped)
        {
            if (g_blackbox.empty())
                g_blackbox.resize(kBlackboxFrames * kVehicleBytes);
            std::memcpy(g_blackbox.data() + (g_blackboxNext % kBlackboxFrames) * kVehicleBytes, rdram + kPlayerVehicle,
                        kVehicleBytes);
            ++g_blackboxNext;
            float pos[3];
            std::memcpy(pos, rdram + kPlayerVehicle + 0x260u, sizeof(pos));
            if (!(std::fabs(pos[0]) < 50000.0f && std::fabs(pos[1]) < 50000.0f && std::fabs(pos[2]) < 50000.0f))
            {
                g_blackboxDumped = true;
                if (FILE *out = std::fopen(g_blackboxPath, "wb"))
                {
                    for (uint32_t i = 0; i < kBlackboxFrames; ++i)
                        std::fwrite(g_blackbox.data() + ((g_blackboxNext + i) % kBlackboxFrames) * kVehicleBytes, 1,
                                    kVehicleBytes, out);
                    std::fclose(out);
                }
                std::fprintf(stderr, "[tmb:blackbox] position %g %g %g at frame %u: dumped\n", pos[0], pos[1], pos[2],
                             g_blackboxNext);
            }
        }
        collisCheckHdResults__7Vehicle_0x117ad8(rdram, ctx, runtime);
    }

    void applyTmb(PS2Runtime &runtime)
    {
        // Draw on the GPU (paraLLEl-GS, Vulkan): 20-23 ms per frame in the match against 24-27 with the CPU rasterizer,
        // and the four raster threads' CPU freed. PS2X_GS_RENDERER=cpu keeps the CPU rasterizer; without a Vulkan
        // device the GS falls back to it by itself.
        if (runtime.gsForPresentation().requestParallelRenderer())
            std::cout << "[tmb] GS on the GPU (PS2X_GS_RENDERER=cpu for the CPU rasterizer)" << std::endl;
        // The game's frame rate at the top right while the speed work goes on (F3 hides it, PS2X_SHOW_FPS=0 is off).
        tmb::host::setEnvDefault("PS2X_SHOW_FPS", "1");
        // Rasterize on 4 worker threads (TMB_GS_SERIAL=1 keeps the GS thread alone).
        if (const char *serial = std::getenv("TMB_GS_SERIAL"); !(serial && *serial == '1'))
        {
            tmb::host::setEnvDefault("PS2X_GS_RASTER_THREADS", "4");
            GSCpuBackend::SetAsyncRasterization(true);
        }
        // VIF1 and the VU1 microcode (two thirds of the game thread in the match) run on their own thread
        // (TMB_VIF1_SERIAL=1 keeps them on the game thread). The game waits for them in sceGsSyncPath each frame.
        if (const char *serial = std::getenv("TMB_VIF1_SERIAL"); !(serial && *serial == '1'))
        {
            runtime.memory().setVif1Threaded(true);
            // TMB_GS_LAG=1: ...and finish a frame while the game runs the next. Off by default: bounded to one
            // presented frame (the input latency it adds), it gains nothing measurable; unbounded, its gain was the
            // GS falling several frames behind the controls.
            const char *lag = std::getenv("TMB_GS_LAG");
            g_gsLag = lag && *lag == '1';
            g_gsLagAb = std::getenv("TMB_GS_LAG_AB") != nullptr;
            g_rainAb = std::getenv("TMB_RAIN_LIFT_AB") != nullptr;
            // On by default with 6 ms (in-run A/B in busy stretches: ~+3 fps, frames over 25 ms 1.5-8% against 6-13%,
            // the game at 0.99 of its speed); TMB_VBLANK_HOLD=0 turns it off.
            const char *hold = std::getenv("TMB_VBLANK_HOLD");
            const int holdMs = hold && *hold ? std::atoi(hold) : 6;
            if (holdMs > 0)
            {
                g_vblankHoldConfigured = g_vblankHoldNow = true;
                runtime.eeScheduler().setVBlankHoldLimit(std::chrono::microseconds(holdMs * 1000));
            }
            const std::string ab = std::getenv("TMB_AB") ? std::getenv("TMB_AB") : "";
            if (ab == "hold" || std::getenv("TMB_VBLANK_HOLD_AB"))
                g_ab = AbExperiment::Hold;
            else if (ab == "slice" || std::getenv("TMB_SLICE_AB"))
                g_ab = AbExperiment::Slice;
            else if (ab == "lag")
            {
                g_ab = AbExperiment::Lag;
                g_gsLag = true; // updateGsLag then switches it with the experiment
            }
            else if (ab == "holdlimit")
                g_ab = AbExperiment::HoldLimit;
            else if (ab == "fill")
                g_ab = AbExperiment::Fill;
        }
        if (const char *idle = std::getenv("TMB_TRACE_IDLE"); idle && *idle == '1')
        {
            g_traceIdle = true;
            runtime.eeScheduler().setIdleObserver([](const EeScheduler::IdleRecord &record) { g_frameIdle.push_back(record); });
        }
        runtime.registerFunction(kVblankHandler, &vblankHandlerOverride);
        runtime.registerFunction(kHier, &hierOverride);
        if (tmb::native::enabled())
            runtime.registerFunction(kHierFlushObjQ, &hierFlushObjQOverride);
        if (const char *trace = std::getenv("TMB_TRACE_HIER"); trace && *trace == '1')
        {
            runtime.registerFunction(kHierDmaHandler, &hierDmaHandlerTrace);
        }
        if (const char *trace = std::getenv("TMB_TRACE_SND"); trace && *trace == '1')
        {
            runtime.registerFunction(kSndSendIopCommandAndWait, &sndSendIopCommandAndWaitTrace);
        }
        runtime.registerFunction(kVideoDecFlush, &videoDecFlushOverride);
        if (const char *maxMs = std::getenv("TMB_MAX_FRAME_MS"); maxMs && *maxMs)
            g_maxFrameTicks = std::max(1, std::atoi(maxMs)) * 576;
        runtime.registerFunction(kTimerEndOfRealTimeProcessing, &timerEndOfRealTimeProcessingOverride);
        g_traceFrames = std::getenv("TMB_TRACE_FRAMES") != nullptr;
        if (const char *substeps = std::getenv("TMB_SUBSTEPS"); substeps && *substeps)
            g_maxSubsteps = std::max(1, std::atoi(substeps));
        runtime.registerFunction(kTheGameUpdate, &theGameUpdateOverride);
        if (const char *timeline = std::getenv("TMB_TRACE_TIMELINE"); timeline && *timeline)
        {
            g_timeline.path = timeline;
            if (const char *start = std::getenv("TMB_TRACE_TIMELINE_START"); start && *start)
                g_timeline.startUpdate = static_cast<uint32_t>(std::max(1, std::atoi(start)));
            if (const char *seconds = std::getenv("TMB_TRACE_TIMELINE_SECONDS"); seconds && *seconds)
                g_timeline.durationNs = static_cast<int64_t>(std::max(1.0, std::atof(seconds)) * 1e9);
            runtime.registerFunction(0x00204E28u, &sceGsSyncPathTimeline);
            runtime.registerFunction(0x00204D90u, &sceGsSyncVTimeline);
        }
        runtime.registerFunction(kSubstepReturn, &substepReturn);
        if (const char *path = std::getenv("TMB_TRACE_BLACKBOX"); path && *path)
            g_blackboxPath = path;
        if (const char *path = std::getenv("TMB_TRACE_POS"); path && *path)
            if ((g_posTrace = std::fopen(path, "w")))
                std::setvbuf(g_posTrace, nullptr, _IOLBF, 0);
        if (g_blackboxPath || g_posTrace)
            runtime.registerFunction(0x00117AD8u, &collisCheckHdResultsBlackbox);
        if (const char *period = std::getenv("TMB_TRACE_THREADS"); period && *period)
        {
            startThreadTrace(runtime, std::max(0.5, std::atof(period)));
        }
        // Movie audio arrives as far ahead as the libmpeg HLE demuxes video: by default up to 128 decoded pictures
        // (5 s), far more than the player's 48 KiB ring and 989snd's 24 KiB IOP buffer (about 375 ms) hold. The PCM
        // queue (pcmCallbackOverride) loses nothing either way; a lead of a few pictures keeps it short.
        {
            size_t picturesAhead = 6u;
            if (const char *value = std::getenv("TMB_MOVIE_PICTURES_AHEAD"); value && *value)
                picturesAhead = static_cast<size_t>(std::max(1, std::atoi(value)));
            ps2_stubs::setMpegMaxPicturesAhead(picturesAhead);
        }
        runtime.registerFunction(kVideoDecAbort, &videoDecAbortOverride);
        runtime.registerFunction(kPcmCallback, &pcmCallbackOverride);
        runtime.registerFunction(kAudioDecSendToIop, &audioDecSendToIopOverride);
        runtime.registerFunction(kAudioDecCreate, &audioDecCreateOverride);
        runtime.registerFunction(kAudioDecReset, &audioDecResetOverride);
        runtime.registerFunction(kSwitchThread, &switchThreadOverride);
        runtime.registerFunction(kSwitchThreadReturn, &switchThreadReturn);
        if (const char *trace = std::getenv("TMB_TRACE_FONT"); trace && *trace == '1')
        {
            runtime.registerFunction(kFontDmaFontData, &fontDmaFontDataTrace);
        }
        if (const char *trace = std::getenv("TMB_TRACE_MOVIE"); trace && *trace == '1')
        {
            runtime.registerFunction(kTermAll, &termAllTrace);
            g_traceMovie = true;
        }

        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = findDiscImage(paths.elfDirectory);
        if (paths.cdImage.empty())
        {
            std::cerr << "[tmb] no disc image found: raw sceCdRead calls will fail (set TMB_ISO)" << std::endl;
            return;
        }
        std::cout << "[tmb] disc image: " << paths.cdImage.string() << std::endl;
        PS2Runtime::setIoPaths(paths);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Twisted Metal - Black (Europe)", "SCES_503.60", kTmbPalEntry, 0u, &applyTmb)
