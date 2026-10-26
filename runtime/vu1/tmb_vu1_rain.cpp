// The rain (VU1 microprogram at pc 0x1d40, compiled fragment 39b4d182198a494a), lifted: its per-drop loop
// (pcs 0x1d80-0x1ed0) as native code, RECOMP_NOTES "Lifting the rain".
//
// One pass of the loop draws one drop P as a line, 5 qwords at vi6: 0 the texture-ish vector C (xyz), 1 and 3 the
// colour (alpha computed / the tail's), 2 and 4 the two screen positions (FTOI4). A is P by this frame's
// view-projection (vf1-vf4), B by last frame's (vf11-vf14), C by vf21-vf24; each is divided by its w (DIV, MULq).
// The drop is culled (vi6 stays, vi10 counts it out of the GIF tag) when one of its screen positions is outside
// the CLIP test the FCAND mask keeps, or its alpha (255 - 3.1875 * B.w, as a 0x4b0000xx float) is below vf25.w.
// The positions come from a chain: G (vf5) and H (vf16) wrap into the 80-unit box (FMAND on the SUBs' sign
// flags), the next P is H, the next H is G, the next G is vf10 * vf10.w + G * vf19.
//
// The entry runs every pass but the last with the same PS2 operations (vuAdd4, products, the ±FLT_MAX clamp of the
// compiled fast path, the DIV and FTOI rules), four drops at a time where they do not depend on each other, then
// leaves the last pass to the compiled block. A pass takes 35 cycles plus one per wrap (the IBEQs skip a pair when
// the sign flag is clear; measured on 1.3M passes of the compiled code).
#include "runtime/ps2_vu1_lifted.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <vector>

namespace tmb
{
    // TMB_RAIN_LIFT_AB (tmb_overrides.cpp): the compiled loop instead, and the strips run natively.
    std::atomic<bool> g_rainLiftOff{false};
    std::atomic<uint64_t> g_rainLiftedStrips{0};
}

namespace
{
    using Vu = VU1Interpreter;
    namespace vu = ps2_vu1_detail;

    constexpr uint32_t kLoopPair = 0x1d80u / 8u;
    Vu::CompiledEntry g_compiledLoop = nullptr; // the compiled block the entry stands in for

    // ---- PS2 operations, lane by lane (each lane its own value) ----
    inline __m128 clamp(__m128 value)
    {
        const __m128 maximum = _mm_set1_ps(std::numeric_limits<float>::max());
        return _mm_max_ps(_mm_min_ps(value, maximum), _mm_xor_ps(maximum, _mm_set1_ps(-0.0f)));
    }
    inline __m128 mul(__m128 a, __m128 b) { return clamp(_mm_mul_ps(a, b)); }
    inline __m128 madd(__m128 acc, __m128 a, __m128 b) { return clamp(vu::vuAdd4(acc, _mm_mul_ps(a, b))); }
    inline __m128 add(__m128 a, __m128 b) { return clamp(vu::vuAdd4(a, b)); }
    inline __m128 sub(__m128 a, __m128 b) { return clamp(vu::vuAdd4(a, vu::negate4(b))); }
    template <int kLane>
    inline __m128 spread(__m128 value) { return _mm_shuffle_ps(value, value, _MM_SHUFFLE(kLane, kLane, kLane, kLane)); }
    inline __m128 lane(const float *vector, int index) { return _mm_set1_ps(vector[index]); }

    // normalizeOperand / normalizeResult: denormals to a zero of their sign, magnitudes at most FLT_MAX's.
    inline __m128 normalize(__m128 value)
    {
        const __m128i bits = _mm_castps_si128(value);
        const __m128i magnitudeMask = _mm_set1_epi32(0x7FFFFFFF);
        const __m128i magnitude = _mm_and_si128(bits, magnitudeMask);
        const __m128i normal = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x007FFFFF));
        const __m128i kept = _mm_and_si128(_mm_min_epi32(magnitude, _mm_set1_epi32(0x7F7FFFFF)), normal);
        return _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(magnitudeMask, bits), kept));
    }
    // DIV Q, vf0w, w (Vu1Jit::div): 1 / w, ±FLT_MAX and the D status bit for a zero w. divideByZero: lane mask.
    inline __m128 reciprocal(__m128 w, uint32_t &divideByZero)
    {
        const __m128 den = normalize(w);
        const __m128 zero = _mm_cmpeq_ps(den, _mm_setzero_ps());
        divideByZero |= static_cast<uint32_t>(_mm_movemask_ps(zero));
        const __m128 saturated = _mm_or_ps(_mm_and_ps(den, _mm_set1_ps(-0.0f)), _mm_set1_ps(std::numeric_limits<float>::max()));
        return normalize(_mm_blendv_ps(_mm_div_ps(_mm_set1_ps(1.0f), den), saturated, zero));
    }
    // FTOI4 (vuFloatToInt with 16): truncated, INT_MAX above the range, INT_MIN below.
    inline __m128i ftoi4(__m128 value)
    {
        const __m128 scaled = _mm_mul_ps(value, _mm_set1_ps(16.0f));
        const __m128i truncated = _mm_cvttps_epi32(scaled);
        const __m128 above = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));
        return _mm_castps_si128(_mm_blendv_ps(_mm_castsi128_ps(truncated), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), above));
    }
    // CLIP's six flags of each lane's (x, y, z) against limit (the bits of |w|, Vu1Jit::clip).
    inline __m128i clipFlags(__m128 x, __m128 y, __m128 z, __m128i limit)
    {
        const __m128i signBit = _mm_set1_epi32(static_cast<int>(0x80000000u));
        __m128i flags = _mm_setzero_si128();
        int bit = 1;
        for (__m128 component : {x, y, z})
        {
            const __m128i bits = _mm_castps_si128(component);
            flags = _mm_or_si128(flags, _mm_and_si128(_mm_cmpgt_epi32(bits, limit), _mm_set1_epi32(bit)));
            flags = _mm_or_si128(flags, _mm_and_si128(_mm_cmpgt_epi32(_mm_xor_si128(bits, signBit), limit), _mm_set1_epi32(bit << 1)));
            bit <<= 2;
        }
        return flags;
    }
    // A live FMAC's flags (Vu1Jit::fmacRelaxed): zero, sign, overflow lane masks of a result (before the clamp).
    struct LaneFlags
    {
        uint32_t zero, sign, overflow;
    };
    inline LaneFlags laneFlags(__m128 result, uint32_t active)
    {
        const __m128 clamped = clamp(result);
        return {static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpeq_ps(clamped, _mm_setzero_ps()))) & active,
                static_cast<uint32_t>(_mm_movemask_ps(clamped)) & active,
                static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpneq_ps(clamped, result))) & active};
    }
    inline uint32_t statusBits(const LaneFlags &flags)
    {
        return (flags.zero ? 0x1u : 0u) | (flags.sign ? 0x2u : 0u) | (flags.overflow ? 0x8u : 0u);
    }

    // ---- The loop's constants (registers it reads and never writes) ----
    struct Constants
    {
        __m128 a[4], b[4], c[4]; // A: vf1-vf4, B: vf11-vf14, C: vf21-vf24 (rows, as the products use them)
        __m128 step;             // vf10.w, spread
        __m128 chainAcc;         // MULAw.xyz ACC, vf10, vf10w (pc 0x1e80)
        __m128 chainScale;       // vf19 (MADD.xyz vf5, vf5, vf19)
        __m128 boxLow, boxHigh;  // vf26, vf25
        __m128 offset;           // vf28 (ADD.xyz before each CLIP)
        __m128i clipLimit;       // vf19.w's |bits| (or the largest denormal for a zero w)
        __m128 alphaBase;        // MULA.w ACC, vf0, vf17: vf17.w
        __m128 alphaScale;       // vf18.w
        __m128 alphaFloor;       // vf25.w
        __m128 tailColor;        // vf15 with w from ADDx.w vf15, vf25, vf0x
    };

    // AoS transforms (one position, lanes xyzw), for the values handed to the last pass.
    inline __m128 transformA(const Constants &k, __m128 p)
    {
        __m128 acc = mul(k.a[3], _mm_set1_ps(1.0f));
        acc = madd(acc, k.a[0], spread<0>(p));
        acc = madd(acc, k.a[1], spread<1>(p));
        return madd(acc, k.a[2], spread<2>(p));
    }
    // MAC bits of a result's lane masks (lane 0 = x is bit 3, w bit 0; sign 4-7, overflow 12-15) and its Z/S/O.
    inline uint32_t macBits(const LaneFlags &flags)
    {
        static constexpr uint8_t kReverse4[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
                                                  0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
        return kReverse4[flags.zero] | (kReverse4[flags.sign] << 4) | (kReverse4[flags.overflow] << 12);
    }
    inline uint32_t macStatus(uint32_t mac)
    {
        return ((mac & 0xFu) ? 0x1u : 0u) | ((mac & 0xF0u) ? 0x2u : 0u) | ((mac & 0xF000u) ? 0x8u : 0u);
    }
    // ACC after MULAx, MADDAy, MADDAz (pcs 0x1eb0-0x1ed0), with the MADDAz's MAC flags.
    inline __m128 partialB(const Constants &k, __m128 p, uint32_t &mac)
    {
        __m128 acc = mul(k.b[0], spread<0>(p));
        acc = madd(acc, k.b[1], spread<1>(p));
        const __m128 raw = vu::vuAdd4(acc, _mm_mul_ps(k.b[2], spread<2>(p)));
        mac = macBits(laneFlags(raw, 0xFu));
        return clamp(raw);
    }

    // Per drop, structure of arrays (index: pass). Scratch kept between runs (VU1 runs on one thread).
    struct Drops
    {
        std::vector<float> px, py, pz;
        std::vector<float> a[4], b[4], c[4];
        // The flags of MADDAz ACC, vf13, vf6z (pc 0x1ed0) computing this drop's B, which the pass before writes
        // when it keeps its drop: MAC, and the status bits (Z, S, O).
        std::vector<uint32_t> bMac, bStatus;
        void resize(size_t count)
        {
            const size_t padded = (count + 3u) & ~size_t{3};
            for (auto *v : {&px, &py, &pz})
                v->assign(padded, 0.0f);
            for (auto *set : {&a, &b, &c})
                for (auto &v : *set)
                    v.assign(padded, 0.0f);
            bMac.assign(padded, 0u);
            bStatus.assign(padded, 0u);
        }
    };
    Drops g_drops;

    // Output of one drop, before packing.
    struct DropOut
    {
        alignas(16) int32_t posA[4];
        alignas(16) int32_t posB[4];
        alignas(16) float c[4];
        float alpha;
        bool culled;
    };
    std::vector<DropOut> g_out;

    Vu::StepResult rainLoop(Vu &vu, const Vu::RunContext &context)
    {
        VU1State &s = Vu1Lifted::state(vu);
        const int32_t passes = static_cast<int16_t>(s.vi[4]); // the loop runs until vi4, decremented first, is 0
        if (passes < 2 || passes > 4096 || !Vu1Lifted::canRunAhead(vu, context, 41ull * passes) ||
            tmb::g_rainLiftOff.load(std::memory_order_relaxed))
            return g_compiledLoop(vu, context);
        tmb::g_rainLiftedStrips.fetch_add(1u, std::memory_order_relaxed);
        Vu1Lifted::landQueued(vu);
        const int32_t native = passes - 1;

        Constants k;
        for (int c = 0; c < 4; ++c)
        {
            k.a[c] = _mm_loadu_ps(s.vf[1 + c]);
            k.b[c] = _mm_loadu_ps(s.vf[11 + c]);
            k.c[c] = _mm_loadu_ps(s.vf[21 + c]);
        }
        const __m128 vf10 = _mm_loadu_ps(s.vf[10]);
        k.step = spread<3>(vf10);
        k.chainAcc = mul(vf10, k.step);
        k.chainScale = _mm_loadu_ps(s.vf[19]);
        k.boxLow = _mm_loadu_ps(s.vf[26]);
        k.boxHigh = _mm_loadu_ps(s.vf[25]);
        k.offset = _mm_loadu_ps(s.vf[28]);
        uint32_t wBits = 0;
        std::memcpy(&wBits, &s.vf[19][3], 4);
        k.clipLimit = _mm_set1_epi32((wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF);
        k.alphaBase = mul(_mm_set1_ps(1.0f), lane(s.vf[17], 3));
        k.alphaScale = lane(s.vf[18], 3);
        k.alphaFloor = lane(s.vf[25], 3);
        const __m128 vf15 = _mm_loadu_ps(s.vf[15]);
        k.tailColor = _mm_blend_ps(vf15, add(spread<3>(k.boxHigh), _mm_setzero_ps()), 0x8);

        // Phase 1, sequential: the positions, the wraps (cycles), the SUBs' flags (sticky status bits).
        Drops &d = g_drops;
        d.resize(static_cast<size_t>(native));
        __m128 g = _mm_loadu_ps(s.vf[5]);
        __m128 h = _mm_loadu_ps(s.vf[16]);
        __m128 p = _mm_loadu_ps(s.vf[6]);
        uint64_t cycles = 0;
        uint32_t sticky = 0;
        for (int32_t pass = 0; pass < native; ++pass)
        {
            alignas(16) float position[4];
            _mm_store_ps(position, p);
            d.px[pass] = position[0];
            d.py[pass] = position[1];
            d.pz[pass] = position[2];
            const __m128 low = vu::vuAdd4(h, vu::negate4(k.boxLow)); // SUB.xyz vf0, vf16, vf26
            const __m128 high = vu::vuAdd4(k.boxHigh, vu::negate4(g)); // SUB.xyz vf0, vf25, vf5
            const LaneFlags lowFlags = laneFlags(low, 0x7u), highFlags = laneFlags(high, 0x7u);
            sticky |= statusBits(lowFlags) | statusBits(highFlags);
            cycles += 35u + static_cast<uint32_t>(__builtin_popcount(lowFlags.sign) + __builtin_popcount(highFlags.sign));
            h = _mm_blendv_ps(h, add(h, k.step), _mm_castsi128_ps(_mm_cmplt_epi32(_mm_setzero_si128(),
                _mm_and_si128(_mm_set1_epi32(static_cast<int>(lowFlags.sign)), _mm_setr_epi32(1, 2, 4, 8)))));
            g = _mm_blendv_ps(g, sub(g, k.step), _mm_castsi128_ps(_mm_cmplt_epi32(_mm_setzero_si128(),
                _mm_and_si128(_mm_set1_epi32(static_cast<int>(highFlags.sign)), _mm_setr_epi32(1, 2, 4, 8)))));
            p = h;                                                        // MOVE.xyzw vf6, vf16
            h = g;                                                        // MOVE.xyzw vf16, vf5
            g = _mm_blend_ps(g, madd(k.chainAcc, g, k.chainScale), 0x7); // MADD.xyz vf5, vf5, vf19
        }

        // Phase 2, four drops at a time: the three transforms.
        const __m128 one = _mm_set1_ps(1.0f);
        alignas(16) float ma[4][4], mb[4][4], mc[4][4]; // [row][component]
        for (int r = 0; r < 4; ++r)
        {
            _mm_store_ps(ma[r], k.a[r]);
            _mm_store_ps(mb[r], k.b[r]);
            _mm_store_ps(mc[r], k.c[r]);
        }
        for (int32_t first = 0; first < native; first += 4)
        {
            const __m128 x = _mm_load_ps(&d.px[first]), y = _mm_load_ps(&d.py[first]), z = _mm_load_ps(&d.pz[first]);
            uint32_t mac[4] = {};
            for (int c = 0; c < 4; ++c)
            {
                __m128 acc = mul(_mm_set1_ps(ma[3][c]), one);
                acc = madd(acc, _mm_set1_ps(ma[0][c]), x);
                acc = madd(acc, _mm_set1_ps(ma[1][c]), y);
                _mm_store_ps(&d.a[c][first], madd(acc, _mm_set1_ps(ma[2][c]), z));
                acc = mul(_mm_set1_ps(mb[0][c]), x);
                acc = madd(acc, _mm_set1_ps(mb[1][c]), y);
                const __m128 raw = vu::vuAdd4(acc, _mm_mul_ps(_mm_set1_ps(mb[2][c]), z));
                const LaneFlags flags = laneFlags(raw, 0xFu); // lanes: drops; component c is MAC bit 3 - c
                const uint32_t bit = 1u << (3 - c);
                for (int i = 0; i < 4; ++i)
                    mac[i] |= (((flags.zero >> i) & 1u) ? bit : 0u) | (((flags.sign >> i) & 1u) ? bit << 4 : 0u) |
                              (((flags.overflow >> i) & 1u) ? bit << 12 : 0u);
                _mm_store_ps(&d.b[c][first], madd(clamp(raw), _mm_set1_ps(mb[3][c]), one));
                acc = mul(_mm_set1_ps(mc[0][c]), x);
                acc = madd(acc, _mm_set1_ps(mc[1][c]), y);
                acc = madd(acc, _mm_set1_ps(mc[2][c]), z);
                _mm_store_ps(&d.c[c][first], madd(acc, _mm_set1_ps(mc[3][c]), one));
            }
            for (int i = 0; i < 4; ++i)
            {
                d.bMac[first + i] = mac[i];
                d.bStatus[first + i] = macStatus(mac[i]);
            }
        }
        // The first drop's A and partial B come from the pairs before the entry (vf7, ACC).
        {
            const __m128 b0 = madd(_mm_loadu_ps(s.acc), k.b[3], one);
            alignas(16) float b0v[4];
            _mm_store_ps(b0v, b0);
            for (int c = 0; c < 4; ++c)
            {
                d.a[c][0] = s.vf[7][c];
                d.b[c][0] = b0v[c];
            }
        }

        // Phase 3, four drops at a time: divisions, screen positions, alpha, clipping, culling.
        g_out.resize(static_cast<size_t>(native) + 3u);
        uint32_t divideByZero = 0;
        uint32_t lastMac = 0, lastStatus = 0, lastDivZero = 0;
        uint32_t clip = s.clip;
        for (int32_t first = 0; first < native; first += 4)
        {
            const int32_t count = std::min<int32_t>(4, native - first);
            const uint32_t active = (1u << count) - 1u;
            uint32_t zeroA = 0, zeroB = 0, zeroC = 0;
            const __m128 aw = _mm_load_ps(&d.a[3][first]);
            const __m128 qa = reciprocal(aw, zeroA);
            const __m128 ax = mul(_mm_load_ps(&d.a[0][first]), qa), ay = mul(_mm_load_ps(&d.a[1][first]), qa),
                         az = mul(_mm_load_ps(&d.a[2][first]), qa);
            const __m128 bw = _mm_load_ps(&d.b[3][first]);
            const __m128 qb = reciprocal(bw, zeroB);
            const __m128 alpha = clamp(vu::vuAdd4(k.alphaBase, vu::negate4(_mm_mul_ps(bw, k.alphaScale))));
            __m128 cw = _mm_load_ps(&d.c[3][first]);
            { // MAXx.w vf9, vf9, vf0x: the larger by sign-magnitude key, +0 on ties
                const __m128i bits = _mm_castps_si128(cw);
                const __m128i sign = _mm_srai_epi32(bits, 31);
                const __m128i key = _mm_sub_epi32(_mm_xor_si128(_mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF)), sign), sign);
                cw = _mm_and_ps(cw, _mm_castsi128_ps(_mm_cmpgt_epi32(key, _mm_setzero_si128())));
            }
            const __m128 qc = reciprocal(cw, zeroC);
            const __m128 bx = mul(_mm_load_ps(&d.b[0][first]), qb), by = mul(_mm_load_ps(&d.b[1][first]), qb),
                         bz = mul(_mm_load_ps(&d.b[2][first]), qb);
            const __m128 cx = mul(_mm_load_ps(&d.c[0][first]), qc), cy = mul(_mm_load_ps(&d.c[1][first]), qc);
            const __m128 cz = _mm_load_ps(&d.c[2][first]);
            const __m128i clipA = clipFlags(add(ax, spread<0>(k.offset)), add(ay, spread<1>(k.offset)),
                                            add(az, spread<2>(k.offset)), k.clipLimit);
            const __m128i clipB = clipFlags(add(bx, spread<0>(k.offset)), add(by, spread<1>(k.offset)),
                                            add(bz, spread<2>(k.offset)), k.clipLimit);
            const __m128 fade = vu::vuAdd4(alpha, vu::negate4(k.alphaFloor)); // SUB.w vf0, vf15, vf25
            const LaneFlags fadeFlags = laneFlags(fade, active);
            sticky |= statusBits(fadeFlags);
            divideByZero |= (zeroA | zeroB | zeroC) & active;

            const __m128i ia[3] = {ftoi4(ax), ftoi4(ay), ftoi4(az)};
            const __m128i ib[3] = {ftoi4(bx), ftoi4(by), ftoi4(bz)};
            alignas(16) int32_t pa[3][4], pb[3][4], ca[4], cb[4];
            alignas(16) float c[3][4], al[4], fd[4];
            for (int i = 0; i < 3; ++i)
            {
                _mm_store_si128(reinterpret_cast<__m128i *>(pa[i]), ia[i]);
                _mm_store_si128(reinterpret_cast<__m128i *>(pb[i]), ib[i]);
            }
            _mm_store_ps(c[0], cx);
            _mm_store_ps(c[1], cy);
            _mm_store_ps(c[2], cz);
            _mm_store_ps(al, alpha);
            _mm_store_ps(fd, clamp(fade));
            _mm_store_si128(reinterpret_cast<__m128i *>(ca), clipA);
            _mm_store_si128(reinterpret_cast<__m128i *>(cb), clipB);
            for (int32_t i = 0; i < count; ++i)
            {
                DropOut &out = g_out[static_cast<size_t>(first + i)];
                for (int j = 0; j < 3; ++j)
                {
                    out.posA[j] = pa[j][i];
                    out.posB[j] = pb[j][i];
                    out.c[j] = c[j][i];
                }
                out.posA[3] = out.posB[3] = 0; // MFIR.w vf7/vf8, vi0
                out.alpha = al[i];
                clip = ((((clip << 6) | static_cast<uint32_t>(ca[i])) << 6) | static_cast<uint32_t>(cb[i])) & 0xFFFFFFu;
                const bool faint = ((fadeFlags.sign >> i) & 1u) != 0u;
                out.culled = (clip & 0xBEFu) != 0u || faint;
                const bool zero = ((fadeFlags.zero >> i) & 1u) != 0u, over = ((fadeFlags.overflow >> i) & 1u) != 0u;
                lastMac = (zero ? 0x1u : 0u) | (faint ? 0x10u : 0u) | (over ? 0x1000u : 0u);
                lastStatus = (zero ? 0x1u : 0u) | (faint ? 0x2u : 0u) | (over ? 0x8u : 0u);
                // Kept: the MADDAz starting the next drop's B writes flags (live on that path only), after these.
                const size_t next = static_cast<size_t>(first + i + 1);
                if (!out.culled && next < static_cast<size_t>(native)) // the last pass's: at the handoff
                {
                    lastMac = d.bMac[next];
                    lastStatus = d.bStatus[next];
                    sticky |= lastStatus;
                }
                lastDivZero = ((zeroC >> i) & 1u) != 0u ? 0x20u : 0u; // the pass's last DIV is C's
            }
        }

        // Phase 4: the kept drops into VU memory, as the loop's stores leave it (a culled drop's 5 qwords are all
        // written again by the next pass, at the same vi6).
        int32_t vi6 = s.vi[6], vi10 = s.vi[10];
        const auto storeAt = [&](int32_t qword, __m128 value, int mask)
        {
            uint32_t address = static_cast<uint32_t>(qword) * 16u;
            address &= context.dataSize - 1u;
            if (address + 16u > context.dataSize)
                return;
            float *target = reinterpret_cast<float *>(context.vuData + address);
            const __m128 old = _mm_loadu_ps(target);
            _mm_storeu_ps(target, mask == 0xF ? value : _mm_blend_ps(old, value, mask));
        };
        for (int32_t pass = 0; pass < native; ++pass)
        {
            const DropOut &out = g_out[static_cast<size_t>(pass)];
            if (out.culled)
            {
                vi10 = static_cast<int16_t>(vi10 - 1);
                continue;
            }
            const __m128 tail = pass == 0 ? vf15 : k.tailColor;
            storeAt(vi6 + 3, tail, 0xF);
            storeAt(vi6 + 1, _mm_blend_ps(vf15, _mm_set1_ps(out.alpha), 0x8), 0xF);
            storeAt(vi6 + 2, _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(out.posA))), 0xF);
            storeAt(vi6 + 4, _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(out.posB))), 0xF);
            storeAt(vi6 + 0, _mm_load_ps(out.c), 0x7);
            vi6 = static_cast<int16_t>(vi6 + 5);
        }

        // Hand the last pass to the compiled block: the state the passes leave.
        _mm_storeu_ps(s.vf[5], g);
        _mm_storeu_ps(s.vf[16], h);
        _mm_storeu_ps(s.vf[6], p);
        _mm_storeu_ps(s.vf[7], transformA(k, p));
        uint32_t handoffMac = 0;
        _mm_storeu_ps(s.acc, partialB(k, p, handoffMac));
        if (!g_out[static_cast<size_t>(native - 1)].culled)
        {
            lastMac = handoffMac;
            lastStatus = macStatus(handoffMac);
            sticky |= lastStatus;
        }
        _mm_storeu_ps(s.vf[15], k.tailColor);
        s.vi[4] = static_cast<int16_t>(s.vi[4] - native);
        s.vi[6] = vi6;
        s.vi[10] = vi10;
        Vu1Lifted::setClip(vu, clip);
        s.mac = lastMac;
        uint32_t divSticky = divideByZero ? 0x20u : 0u;
        s.status = (s.status & 0xFC0u) | lastStatus | lastDivZero | ((sticky | divSticky) << 6);
        Vu1Lifted::advanceTo(vu, Vu1Lifted::cycle(vu) + cycles);
        return g_compiledLoop(vu, context);
    }

    const bool g_registered = (Vu::registerLiftedEntry({0u, 1284u, 0x39b4d182198a494aull, kLoopPair, &rainLoop, &g_compiledLoop}), true);
}
