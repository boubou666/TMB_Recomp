// TMB's VU0 microprograms in C++: the programs the EE calls with VCALLMS, run here instead of on a VU0 interpreter
// (ps2x::native::Vu0Hle). Each one does what the microcode does to the VU0 registers (the EE context's vu0_vf, vi,
// ACC, Q, I and clip flags) and to VU0 data memory, in the microcode's order: VU0 stalls on a vector register still
// being written, so its programs read as sequential code, but for the two instructions of a pair (both read their
// operands before either writes) and the Q register (a DIV's result arrives 7 cycles on; every MULq here comes after
// it). PS2X_VU0_HLE_CHECK=1 compares each program with the interpreter.
//
// The 2D image (ELF 0x1347e0, loaded at 0 by the 2D drawing code): 0x278 (drawcSetupAsm: the transforms), 0x548 and
// 0x5e0 (drawcFirstPtAsm, drawcRenderAsm: a strip of quads along points, drawNonTexSprite), 0x3b0 and 0x438
// (drawcFirstLineAsm, drawcRenderLinesAsm: lines through points, drawLines). TMB_VU0_NATIVE=0: all on the interpreter.

#include "ps2_recomp_abi.h"
#include "runtime/native/native_vu0_hle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>

namespace tmb::vu0
{
    namespace
    {
        struct Vec
        {
            float v[4] = {0, 0, 0, 0};
            float &operator[](int i) { return v[i]; }
            float operator[](int i) const { return v[i]; }
        };

        // VU0 as the programs use it: registers loaded from the context, written back at the end.
        struct Vu
        {
            Vec vf[32];
            int32_t vi[16] = {};
            Vec acc;
            float q = 1.0f, i = 0.0f;
            uint32_t clip = 0;
            uint8_t *data = nullptr;

            explicit Vu(const R5900Context *ctx, uint8_t *vu0Data) : data(vu0Data)
            {
                for (int r = 0; r < 32; ++r)
                    _mm_storeu_ps(vf[r].v, ctx->vu0_vf[r]);
                vf[0] = Vec{{0, 0, 0, 1}};
                for (int r = 0; r < 16; ++r)
                    vi[r] = static_cast<int16_t>(ctx->vi[r]);
                vi[0] = 0;
                _mm_storeu_ps(acc.v, ctx->vu0_acc);
                q = ctx->vu0_q;
                i = ctx->vu0_i;
                clip = ctx->vu0_clip_flags;
            }
            void store(R5900Context *ctx) const
            {
                for (int r = 1; r < 32; ++r)
                    ctx->vu0_vf[r] = _mm_loadu_ps(vf[r].v);
                for (int r = 1; r < 16; ++r)
                    ctx->vi[r] = static_cast<uint16_t>(vi[r]);
                ctx->vu0_acc = _mm_loadu_ps(acc.v);
                ctx->vu0_q = q;
                ctx->vu0_i = i;
                ctx->vu0_clip_flags = clip;
            }

            float *mem(int32_t qword) { return reinterpret_cast<float *>(data + (static_cast<uint32_t>(qword) & 0xffu) * 16u); }
            Vec lq(int32_t qword)
            {
                Vec out;
                std::memcpy(out.v, mem(qword), 16);
                return out;
            }
            void sq(const Vec &value, int32_t qword) { std::memcpy(mem(qword), value.v, 16); }
            int32_t ilw(int32_t qword, int lane) // ILWR: the field's low 16 bits
            {
                uint32_t word;
                std::memcpy(&word, reinterpret_cast<uint8_t *>(mem(qword)) + lane * 4, 4);
                return static_cast<int32_t>(word & 0xffffu);
            }

            // ACC = a * b[lane] + c * d[lane] + e * f[lane] (the MULA / MADDA chains), xyzw.
            static Vec combine(const Vec &a, float x, const Vec &b, float y, const Vec &c, float z)
            {
                Vec out;
                for (int k = 0; k < 4; ++k)
                    out[k] = a[k] * x + b[k] * y + c[k] * z;
                return out;
            }
            static Vec madd(const Vec &accumulated, const Vec &a, float w)
            {
                Vec out;
                for (int k = 0; k < 4; ++k)
                    out[k] = accumulated[k] + a[k] * w;
                return out;
            }
            // CLIP fs.xyz, ft.w: the judgment shifted into the clip flags.
            void clipTest(const Vec &p)
            {
                const float w = std::fabs(p[3]);
                uint32_t j = 0;
                j |= p[0] > w ? 0x01u : 0u;
                j |= p[0] < -w ? 0x02u : 0u;
                j |= p[1] > w ? 0x04u : 0u;
                j |= p[1] < -w ? 0x08u : 0u;
                j |= p[2] > w ? 0x10u : 0u;
                j |= p[2] < -w ? 0x20u : 0u;
                clip = ((clip << 6) | j) & 0xffffffu;
            }
        };

        float asFloat(int32_t bits)
        {
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        int32_t asInt(float f)
        {
            int32_t bits;
            std::memcpy(&bits, &f, 4);
            return bits;
        }
        // FTOI0 / FTOI4: to fixed point with 0 / 4 fraction bits, toward zero, saturating; the bits stay in the field.
        float ftoi(float value, float scale)
        {
            const double scaled = static_cast<double>(value) * scale;
            const int32_t i = scaled >= 2147483647.0 ? 2147483647 : scaled <= -2147483648.0 ? INT32_MIN : static_cast<int32_t>(scaled);
            return asFloat(i);
        }
        float mini(float a, float b) { return std::min(a, b); }
        float maxi(float a, float b) { return std::max(a, b); }

        // The MAC flags an xyz FMAC write leaves (zero bits 3..1, sign bits 7..5 for x..z).
        uint32_t macFlags(const Vec &v)
        {
            uint32_t flags = 0;
            for (int k = 0; k < 3; ++k)
            {
                if (v[k] == 0.0f)
                    flags |= 0x8u >> k;
                if (std::signbit(v[k]) && v[k] != 0.0f)
                    flags |= 0x80u >> k;
            }
            return flags;
        }

        // ---- the 2D image ----

        // 0x278: vf1-4 = the matrix at VU 238-241 by the one in vf17-20 (from drawcSetupAsm), vf13-16 the one at VU
        // 242-245 by it; vf11 = VU 249 (the colour offset), vf18 = VU 250, VU 255 = VU 238, vf21.w = -8.
        void setup2d(Vu &u)
        {
            Vec *f = u.vf;
            f[9] = u.lq(238);
            f[10] = u.lq(239);
            f[11] = u.lq(240);
            f[12] = u.lq(241);
            const Vec m[4] = {f[17], f[18], f[19], f[20]};
            for (int k = 0; k < 4; ++k)
                f[1 + k] = Vu::madd(Vu::combine(f[9], m[k][0], f[10], m[k][1], f[11], m[k][2]), f[12], m[k][3]);
            f[25] = u.lq(242);
            f[26] = u.lq(243);
            f[27] = u.lq(244);
            f[28] = u.lq(245);
            u.vi[4] = 0x7fff;
            u.vi[13] = 0x8000;
            for (int k = 0; k < 4; ++k)
            {
                u.acc = Vu::combine(f[25], m[k][0], f[26], m[k][1], f[27], m[k][2]);
                f[13 + k] = Vu::madd(u.acc, f[28], m[k][3]);
            }
            f[11] = u.lq(249);
            f[18] = u.lq(250);
            u.i = -8.0f;
            u.sq(f[9], 255);
            f[21][3] = -8.0f;
        }

        // vf25 = vf1-4 by vf20 with w 1 (0x548, 0x5e0) or vf20.w (the lines); vf5 the same by vf13-16.
        Vec project(const Vec *f, int first, const Vec &p, float w)
        {
            return Vu::madd(Vu::combine(f[first], p[0], f[first + 1], p[1], f[first + 2], p[2]), f[first + 3], w);
        }

        // 0x5e0 .. 0x728: one point of the strip (drawcRenderAsm), the point loaded before it projected (vf25, vf5,
        // its colours vf8 / vf9 and texture coordinates from VU vi6): vf31 / vf17 its two vertices (FTOI4, half the
        // width VU vi3.w * 608 / w to each side in x), vf28 / vf29 their STQ, vf10 / vf12 their colours, vi11 whether
        // it was outside the clip volume; the next point loaded and projected.
        void stripPoint(Vu &u)
        {
            Vec *f = u.vf;
            f[26] = u.lq(u.vi[6]++);
            f[27] = u.lq(u.vi[6]++);
            u.clipTest(f[5]);
            for (int k = 0; k < 4; ++k)
                f[8][k] = mini(f[8][k], 255.0f);
            for (int k = 0; k < 4; ++k)
                f[24][k] = f[25][k] * u.q;
            for (int k = 0; k < 3; ++k)
                f[26][k] *= u.q;
            f[7][3] = f[20][3] * u.q;
            u.vi[1] = (u.clip & 0x3fu) != 0u ? 1 : 0;
            f[20] = u.lq(u.vi[3]++);
            f[24][2] *= f[30][0];
            for (int k = 0; k < 3; ++k)
                f[27][k] *= u.q;
            for (int k = 0; k < 4; ++k)
                f[9][k] = mini(f[9][k], 255.0f);
            for (int k = 0; k < 4; ++k)
                f[8][k] = maxi(f[8][k], 0.0f);
            f[24][3] = 255.0f;
            f[7][3] *= 608.0f;
            for (int k = 0; k < 4; ++k)
                f[9][k] = maxi(f[9][k], 0.0f);
            for (int k = 0; k < 4; ++k)
                f[10][k] = ftoi(f[8][k], 1.0f);
            f[17] = f[24];
            f[24][0] -= f[7][3];
            for (int k = 0; k < 4; ++k)
                f[12][k] = ftoi(f[9][k], 1.0f);
            f[8] = u.lq(u.vi[5]++);
            f[9] = u.lq(u.vi[5]++);
            f[17][0] += f[7][3];
            for (int k = 0; k < 2; ++k)
                f[24][k] = mini(f[24][k], 4095.0f);
            f[25] = project(f, 1, f[20], 1.0f);
            for (int k = 0; k < 4; ++k)
                f[8][k] += f[11][k];
            for (int k = 0; k < 2; ++k)
                f[17][k] = mini(f[17][k], 4095.0f);
            for (int k = 0; k < 2; ++k)
                f[24][k] = maxi(f[24][k], 0.0f);
            const float nextQ = 1.0f / f[25][3];
            for (int k = 0; k < 4; ++k)
                f[9][k] += f[11][k];
            for (int k = 0; k < 2; ++k)
                f[17][k] = maxi(f[17][k], 0.0f);
            f[29] = f[27];
            for (int k = 0; k < 4; ++k)
                f[31][k] = ftoi(f[24][k], 16.0f);
            f[28] = f[26];
            f[20][1] -= 2.0f;
            for (int k = 0; k < 4; ++k)
                f[17][k] = ftoi(f[17][k], 16.0f);
            u.vi[11] = u.vi[1];
            u.acc = Vu::combine(f[13], f[20][0], f[14], f[20][1], f[15], f[20][2]);
            f[5] = Vu::madd(u.acc, f[16], 1.0f);
            u.q = nextQ;
            u.i = 255.0f;
        }

        // 0x548: the strip's first point (drawcFirstPtAsm: vi2 the point count, vi10 twice it): VU 1 projected, its
        // colours (VU vi2 + 1 on) and texture coordinates (VU vi10 + vi2 + 1 on) loaded, then as 0x5e0.
        void stripFirst(Vu &u)
        {
            Vec *f = u.vf;
            f[20] = u.lq(1);
            u.vi[1] = 1;
            u.vi[5] = u.vi[2] + u.vi[1];
            u.vi[3] = 2;
            u.vi[6] = u.vi[10] + u.vi[5];
            f[25] = project(f, 1, f[20], 1.0f);
            f[8] = u.lq(u.vi[5]++);
            f[9] = u.lq(u.vi[5]++);
            f[20][1] -= 2.0f;
            for (int k = 0; k < 4; ++k)
                f[8][k] += f[11][k];
            for (int k = 0; k < 4; ++k)
                f[9][k] += f[11][k];
            u.q = 1.0f / f[25][3];
            u.acc = Vu::combine(f[13], f[20][0], f[14], f[20][1], f[15], f[20][2]);
            f[5] = Vu::madd(u.acc, f[16], 1.0f);
            stripPoint(u);
        }

        // 0x438 .. 0x540: one point of the lines (drawcRenderLinesAsm): the point projected before (vf25, vf5) as
        // vf27 (FTOI4, w the Z 255 with the ADC bit when it or one of the two before was outside the clip volume) and
        // vf26 its colour; the next point loaded (VU vi3) and projected.
        void linePoint(Vu &u)
        {
            Vec *f = u.vf;
            f[9] = u.lq(u.vi[6]++);
            f[12] = u.lq(u.vi[5]++);
            u.clipTest(f[5]);
            f[20] = u.lq(u.vi[3]++);
            for (int k = 0; k < 4; ++k)
                f[24][k] = f[25][k] * u.q;
            u.i = 255.0f;
            for (int k = 0; k < 3; ++k)
                f[9][k] *= u.q;
            for (int k = 0; k < 4; ++k)
                f[12][k] += f[11][k];
            f[6][3] = f[18][3];
            u.vi[1] = (u.clip & 0x3ffffu) != 0u ? 1 : 0;
            for (int k = 0; k < 3; ++k)
                f[8][k] = f[24][k] - f[23][k];
            f[23] = f[24];
            f[24][2] *= f[30][0];
            u.vi[11] = u.vi[2];
            for (int k = 0; k < 4; ++k)
                f[12][k] = mini(f[12][k], 255.0f);
            f[31][0] = -f[31][0];
            // OPMULA / OPMSUB: vf10 = vf8 x vf7 (xyz)
            const Vec a = f[8], b = f[7];
            u.acc[0] = a[1] * b[2];
            u.acc[1] = a[2] * b[0];
            u.acc[2] = a[0] * b[1];
            u.vi[1] &= u.vi[7];
            f[10][0] = u.acc[0] - b[1] * a[2];
            f[10][1] = u.acc[1] - b[2] * a[0];
            f[10][2] = u.acc[2] - b[0] * a[1];
            const uint32_t mac = macFlags(f[10]);
            for (int k = 0; k < 4; ++k)
                f[12][k] = maxi(f[12][k], 0.0f);
            f[24][3] = 255.0f;
            u.vi[10] -= 1;
            u.vi[14] = static_cast<int32_t>(mac) & u.vi[11];
            for (int k = 0; k < 4; ++k)
                f[12][k] = ftoi(f[12][k], 1.0f);
            f[25] = project(f, 1, f[20], f[20][3]);
            u.vi[14] = u.vi[1] + u.vi[14];
            for (int k = 0; k < 4; ++k)
                f[27][k] = ftoi(f[24][k], 16.0f);
            const float s = f[31][0];
            for (int k = 0; k < 4; ++k)
                f[7][k] = f[8][k] * s;
            f[17] = f[9];
            f[26] = f[12];
            f[19][3] = f[25][3] - f[30][3];
            const float nextQ = 1.0f / f[25][3];
            u.vi[9] = asInt(f[27][3]) & 0xffff;
            u.acc = Vu::combine(f[13], f[20][0], f[14], f[20][1], f[15], f[20][2]);
            f[5] = Vu::madd(u.acc, f[16], f[20][3]);
            if (u.vi[14] != 0)
                u.vi[9] |= u.vi[13];
            f[27][3] = asFloat(static_cast<int16_t>(u.vi[9]));
            u.q = nextQ;
        }

        // 0x3b0: the lines' first point (drawcFirstLineAsm: vi7 0xfff, vi2 0): the GIF tag at VU 0 copied to VU
        // (VU 0.w), VU 1 projected, the clip flags cleared, then as 0x438.
        void lineFirst(Vu &u)
        {
            Vec *f = u.vf;
            u.vi[1] = 0;
            f[20] = u.lq(1);
            u.vi[10] = u.ilw(0, 0);
            f[31][0] = -f[30][2];
            u.vi[12] = u.ilw(0, 3);
            u.vi[3] = 1;
            f[23] = u.lq(0);
            u.vi[10] &= u.vi[4];
            u.vi[12] += u.vi[1];
            f[25] = project(f, 1, f[20], f[20][3]);
            u.vi[5] = u.vi[3] + u.vi[10];
            u.vi[5] += u.vi[10];
            u.vi[6] = u.vi[5] + u.vi[10];
            u.clip = 0;
            u.q = 1.0f / f[25][3];
            u.acc = Vu::combine(f[13], f[20][0], f[14], f[20][1], f[15], f[20][2]);
            f[5] = Vu::madd(u.acc, f[16], f[20][3]);
            u.sq(f[23], u.vi[12]);
            u.vi[3] = u.vi[1] + 2;
            linePoint(u);
        }

        // VU0's code memory holds the 2D image (the pairs at 0x278, 0x548 and 0x720 as the ELF has them).
        bool is2dImage(const uint8_t *code)
        {
            static constexpr uint32_t kPairs[][3] = {{0x278u, 0x01e900eeu, 0x000002ffu}, {0x548u, 0x01f40001u, 0x000002ffu},
                                                     {0x720u, 0x437f0000u, 0xc00002ffu}, {0x3b0u, 0x10010000u, 0x000002ffu}};
            for (const auto &pair : kPairs)
            {
                uint32_t words[2];
                std::memcpy(words, code + pair[0], 8);
                if (words[0] != pair[1] || words[1] != pair[2])
                    return false;
            }
            return true;
        }

        // ---- the hierarchy image (ELF 0x135070, loaded at 0 for the scene traversal: hier, hierTraverseAsm) ----

        bool negative(float v) { return std::signbit(v) && v != 0.0f; }

        // a * m.x + b * m.y + c * m.z over four lanes (the MULA / MADDA / MADD chains).
        Vec by3(const Vec &a, const Vec &b, const Vec &c, const Vec &m) { return Vu::combine(a, m[0], b, m[1], c, m[2]); }

        // The 3x3 transpose the programs make of three registers in place (diagonals and w kept).
        void transpose3(Vec &a, Vec &b, Vec &c)
        {
            std::swap(a[1], b[0]);
            std::swap(a[2], c[0]);
            std::swap(b[2], c[1]);
        }

        // 0x278 (hierFovTest2Asm): an object's bounds against the view: its centre vf4 + vf5 (vf5.w its radius) in the
        // frames at VU vi10 + 50 (vf30) and + 53 (vf29), each pushed out and in by the radius; vi2 / vi5 the sign flags
        // the microcode reads of them (FMAND / FSAND), vf10 = VU 0 with x + the far corner's z.
        void fovTest(Vu &u)
        {
            Vec *f = u.vf;
            const int32_t b = u.vi[10];
            u.i = 1.0f;
            for (int k = 0; k < 3; ++k)
                f[31][k] = f[5][k] + f[4][k];
            f[7] = u.lq(b + 53);
            f[6][0] = f[5][3];
            f[8] = u.lq(b + 54);
            f[11][3] = f[5][3] * u.i;
            f[9] = u.lq(b + 55);
            f[1] = u.lq(b + 50);
            f[2] = u.lq(b + 51);
            f[3] = u.lq(b + 52);
            f[29] = by3(f[7], f[8], f[9], f[31]);
            u.acc = by3(f[1], f[2], f[3], f[31]);
            f[30] = u.acc;
            f[10] = u.lq(0);
            u.vi[6] = 0xc0;
            u.vi[7] = 0x80;
            for (int k = 0; k < 3; ++k)
            {
                f[27][k] = f[29][k] + f[6][0];
                f[25][k] = f[29][k] - f[11][3];
                f[28][k] = f[30][k] + f[6][0];
                f[26][k] = f[30][k] - f[11][3];
            }
            const auto signsXy = [](const Vec &v) { return (negative(v[0]) ? 0x80 : 0) | (negative(v[1]) ? 0x40 : 0); };
            const auto anySign = [](const Vec &v) { return negative(v[0]) || negative(v[1]) || negative(v[2]) ? 0x2 : 0; };
            u.vi[3] = signsXy(f[27]) & u.vi[6];
            u.vi[4] = signsXy(f[25]) & u.vi[6];
            f[10][0] = f[10][0] + f[27][2];
            u.vi[2] = anySign(f[28]);
            u.vi[5] = anySign(f[26]);
            u.vi[2] |= u.vi[3];
            u.vi[3] = negative(f[10][0]) ? 0x80 : 0;
            u.vi[2] |= u.vi[3];
            u.vi[5] |= u.vi[4];
        }

        // 0x3a8: vf3 = vf1 + vf4, vf6.x = |vf3|^2 * vf8.y.
        void scaledDistance(Vu &u)
        {
            Vec *f = u.vf;
            for (int k = 0; k < 3; ++k)
                f[3][k] = f[1][k] + f[4][k];
            for (int k = 0; k < 3; ++k)
                f[7][k] = 1.0f;
            for (int k = 0; k < 3; ++k)
                f[5][k] = f[3][k] * f[3][k];
            u.acc[0] = f[3][0] * f[3][0] + f[7][0] * f[5][1];
            f[6][0] = u.acc[0] + f[7][0] * f[5][2];
            f[6][0] *= f[8][1];
        }

        // 0x410 (hierMulEoForObjAsm): vf6 = the rotation at VU vi10 + 59 by vf4 (w 1); vf7-9 = VU vi10 + 56..58.
        void rotatePoint(Vu &u)
        {
            Vec *f = u.vf;
            const int32_t b = u.vi[10];
            f[6][3] = 1.0f;
            f[1] = u.lq(b + 59);
            f[2] = u.lq(b + 60);
            f[3] = u.lq(b + 61);
            for (int k = 0; k < 3; ++k)
                u.acc[k] = f[1][k] * f[4][0] + f[2][k] * f[4][1];
            for (int k = 0; k < 3; ++k)
                f[6][k] = u.acc[k] + f[3][k] * f[4][2];
            f[7] = u.lq(b + 56);
            f[8] = u.lq(b + 57);
            f[9] = u.lq(b + 58);
        }

        // m[0] * a.x + m[1] * a.y + m[2] * a.z over four lanes, ACC left as the microcode's MADDAy leaves it.
        Vec product3(Vu &u, const Vec (&m)[3], const Vec &a)
        {
            for (int k = 0; k < 4; ++k)
                u.acc[k] = m[0][k] * a[0] + m[1][k] * a[1];
            Vec out;
            for (int k = 0; k < 4; ++k)
                out[k] = u.acc[k] + m[2][k] * a[2];
            return out;
        }

        // 0x450 (hierCsUpdateAsm): the camera rotation vf11-13 stored at VU 59-61 and its transpose by vf31 = VU 10 +
        // vf19 into vf4; VU 50-58 = the three frames at VU 11-13, 15-17, 19-21 by the rotation.
        void cameraUpdate(Vu &u)
        {
            Vec *f = u.vf;
            f[17] = f[12];
            f[16] = f[11];
            f[15] = f[13];
            transpose3(f[11], f[12], f[13]);
            f[18] = u.lq(10);
            f[20] = u.lq(11);
            f[21] = u.lq(12);
            f[22] = u.lq(13);
            f[23] = u.lq(15);
            f[24] = u.lq(16);
            f[25] = u.lq(17);
            f[26] = u.lq(19);
            for (int k = 0; k < 3; ++k)
                f[31][k] = f[18][k] + f[19][k];
            const Vec t = by3(f[11], f[12], f[13], f[31]);
            for (int k = 0; k < 3; ++k)
                f[4][k] = t[k];
            u.sq(f[16], 59);
            f[28] = u.lq(21);
            f[27] = u.lq(20);
            u.sq(f[17], 60);
            u.sq(f[15], 61);
            const Vec rows[3] = {f[16], f[17], f[15]};
            const Vec frames[3][3] = {{f[20], f[21], f[22]}, {f[23], f[24], f[25]}, {f[26], f[27], f[28]}};
            Vec out[9];
            for (int g = 0; g < 3; ++g)
                for (int r = 0; r < 3; ++r)
                    out[g * 3 + r] = product3(u, frames[g], rows[r]);
            const int regs[9] = {11, 12, 13, 20, 21, 22, 23, 24, 25};
            for (int n = 0; n < 9; ++n)
            {
                f[regs[n]] = out[n];
                u.sq(out[n], 50 + n);
            }
        }

        // 0x5a8 (hier): the root's frames: VU 50-58 = VU 11-13, 15-17, 19-21, VU 59-61 the identity rotation, vf4 = VU 10.
        void hierRoot(Vu &u)
        {
            Vec *f = u.vf;
            f[26] = Vec{{0, 0, 1, 0}};
            f[27] = Vec{{0, 1, 0, 0}};
            f[28] = Vec{{1, 0, 0, 0}};
            const int regs[9] = {11, 12, 13, 20, 21, 22, 23, 24, 25};
            const int from[9] = {11, 12, 13, 15, 16, 17, 19, 20, 21};
            for (int n = 0; n < 9; ++n)
                f[regs[n]] = u.lq(from[n]);
            f[4] = u.lq(10);
            for (int n = 0; n < 9; ++n)
                u.sq(f[regs[n]], 50 + n);
            u.sq(f[28], 59);
            u.sq(f[27], 60);
            u.sq(f[26], 61);
        }

        // 0x670 (vu0HierRotateAsm): a child's frames: the parent's at VU vi10 + 50..61 by the rotation vf1-3, stored
        // twelve on (vi10 += 12); vf4 = the rotation's transpose by vf31 = vf4 + vf5.
        void hierRotate(Vu &u)
        {
            Vec *f = u.vf;
            for (int k = 0; k < 3; ++k)
                f[31][k] = f[4][k] + f[5][k];
            f[6] = f[1];
            f[7] = f[2];
            f[8] = f[3];
            transpose3(f[1], f[2], f[3]);
            const int32_t b = u.vi[10];
            const Vec r[3] = {u.lq(b + 59), u.lq(b + 60), u.lq(b + 61)};
            const Vec g[3][3] = {{u.lq(b + 50), u.lq(b + 51), u.lq(b + 52)}, {u.lq(b + 53), u.lq(b + 54), u.lq(b + 55)},
                                 {u.lq(b + 56), u.lq(b + 57), u.lq(b + 58)}};
            const Vec t = by3(f[1], f[2], f[3], f[31]);
            for (int k = 0; k < 3; ++k)
                f[4][k] = t[k];
            const Vec axes[3] = {f[6], f[7], f[8]};
            u.vi[10] = b + 12;
            const int32_t n = u.vi[10];
            Vec rot[3], frame[3][3];
            for (int a = 0; a < 3; ++a)
                rot[a] = product3(u, r, axes[a]);
            for (int m = 0; m < 3; ++m)
                for (int a = 0; a < 3; ++a)
                    frame[m][a] = product3(u, g[m], axes[a]);
            f[21] = rot[0];
            f[22] = rot[1];
            f[23] = rot[2];
            f[25] = frame[0][0];
            f[26] = frame[0][1];
            f[27] = frame[0][2];
            f[9] = frame[1][0];
            f[10] = frame[1][1];
            f[11] = frame[1][2];
            f[28] = frame[2][0];
            f[29] = frame[2][1];
            f[30] = frame[2][2];
            f[13] = g[0][0];
            f[14] = g[0][1];
            f[15] = g[0][2];
            f[17] = g[1][0];
            f[18] = g[1][1];
            f[19] = g[1][2];
            f[12] = g[2][0];
            f[16] = g[2][1];
            f[20] = g[2][2];
            for (int a = 0; a < 3; ++a)
                u.sq(rot[a], n + 59 + a);
            for (int m = 0; m < 3; ++m)
                for (int a = 0; a < 3; ++a)
                    u.sq(frame[m][a], n + 50 + m * 3 + a);
        }

        // 0x808 (hierTraverseAsm): the lights reaching an object: of the VU 9.x spheres (VU 22 + i, VU 30 + i; at most
        // four kept), each nearer the object's centre vf4 + vf5 than their radii: VU 1 + n its direction in the object's
        // frame over 1 - w (vf0 for the ambient ones, VU 9.y's mask, or none when it faces away), VU 5 + n vf4 minus its
        // centre, VU 45 + n VU 36 + i; then the light matrix (vf10, vf18, vf12, vf13) and colours (vf20-22, vf28) from
        // them, the other registers as the microcode leaves them.
        void lights(Vu &u)
        {
            Vec *f = u.vf;
            f[18] = Vec{};
            u.vi[14] = 0;
            f[11] = f[1];
            f[21] = u.lq(10);
            f[12] = f[2];
            f[20] = u.lq(22);
            f[13] = f[3];
            u.vi[5] = u.ilw(9, 0);
            f[17] = f[2];
            f[16] = f[1];
            f[11][2] = f[3][0];
            u.sq(f[18], 1);
            f[12][2] = f[3][1];
            f[15] = f[13];
            f[13][0] = f[16][2];
            u.vi[8] = 0;
            f[11][1] = f[17][0];
            f[22] = u.lq(30);
            f[12][0] = f[16][1];
            u.sq(f[18], 2);
            f[13][1] = f[17][2];
            u.vi[3] = 1;
            for (int k = 0; k < 4; ++k)
                f[14][k] = f[4][k] + f[5][k];
            u.vi[9] = 4;
            f[19] = Vec{{1, 1, 1, 2}};
            for (int at : {3, 4, 45, 46, 47, 48})
                u.sq(f[18], at);
            u.vi[4] = 1;
            u.vi[2] = u.ilw(9, 1);
            for (int guard = 0; guard < 4096; ++guard)
            {
                for (int k = 0; k < 4; ++k)
                    f[17][k] = f[21][k] + f[20][k];
                u.i = -1.0f;
                f[10] = u.lq(1);
                f[15][3] = 1.0f - f[22][3];
                const bool noneLeft = u.vi[5] == 0;
                f[24][3] = f[22][3] * u.i;
                f[18] = u.lq(2);
                if (noneLeft)
                    break;
                if (u.vi[9] == 0)
                {
                    for (int k = 0; k < 3; ++k)
                        u.acc[k] = f[11][k] * f[17][0] + f[12][k] * f[17][1];
                    break;
                }
                for (int k = 0; k < 3; ++k)
                    f[23][k] = f[11][k] * f[17][0] + f[12][k] * f[17][1] + f[13][k] * f[17][2];
                u.vi[5] -= 1;
                u.q = 1.0f / f[15][3];
                for (int k = 0; k < 3; ++k)
                {
                    u.acc[k] = f[11][k] * f[22][0] + f[12][k] * f[22][1];
                    f[24][k] = u.acc[k] + f[13][k] * f[22][2];
                }
                u.vi[6] = 0x80;
                for (int k = 0; k < 4; ++k)
                    f[26][k] = f[14][k] - f[23][k];
                for (int k = 0; k < 4; ++k)
                    f[27][k] = f[4][k] - f[23][k];
                u.vi[10] = 0x40;
                u.acc[3] = f[20][3] * f[20][3];
                f[28][3] = u.acc[3] + f[5][3] * f[5][3];
                for (int k = 0; k < 4; ++k)
                    f[15][k] = f[26][k] * f[26][k];
                f[20] = u.lq(22 + u.vi[3]);
                for (int k = 0; k < 4; ++k)
                    f[16][k] = f[26][k] * f[24][k];
                f[22] = u.lq(30 + u.vi[3]);
                for (int k = 0; k < 4; ++k)
                    f[10][k] = f[24][k] * u.q;
                u.acc[0] = f[26][0] * f[26][0];
                u.acc[0] += f[10][0] * f[15][1];
                u.acc[1] = f[16][1] + f[16][0];
                u.vi[3] += 1;
                u.acc[0] += f[19][0] * f[15][2];
                u.vi[12] = u.vi[4] & u.vi[2];
                f[15][0] = u.acc[0] - f[19][0] * f[28][3];
                u.acc[1] += f[19][1] * f[16][2];
                u.vi[4] = u.vi[4] + u.vi[4];
                f[16][1] = u.acc[1] + f[19][1] * f[5][3];
                u.vi[7] = negative(f[15][0]) ? 0x80 : 0;
                const bool inside = u.vi[7] != 0;
                f[15] = u.lq(36 + u.vi[3]);
                if (!inside)
                    continue;
                u.vi[7] = negative(f[16][1]) ? 0x40 : 0;
                if (u.vi[12] != 0)
                    u.sq(f[0], 1 + u.vi[8]);
                else if (u.vi[7] != 0)
                    continue;
                else
                    u.sq(f[10], 1 + u.vi[8]);
                u.sq(f[27], 5 + u.vi[8]);
                u.sq(f[15], 45 + u.vi[8]);
                u.vi[8] += 1;
                u.vi[14] = 1;
                u.vi[9] -= 1;
            }
            // L_09f8: the ones kept, as the light matrix and colours
            f[13][3] = 0.0f;
            f[20] = u.lq(45);
            f[22] = f[18];
            f[21] = f[10];
            f[12] = u.lq(3);
            f[14] = u.lq(5);
            f[15] = u.lq(6);
            f[16] = u.lq(7);
            f[10][1] = f[18][0];
            f[10][3] = 0.0f;
            f[18][3] = 0.0f;
            f[18][0] = f[21][1];
            f[10][2] = f[12][0];
            f[23] = f[12];
            f[18][2] = f[12][1];
            f[25] = f[15];
            f[26] = f[16];
            f[12][0] = f[21][2];
            f[12][1] = f[22][2];
            f[12][3] = 0.0f;
            f[24] = f[14];
            f[13][0] = f[21][3];
            f[21] = u.lq(46);
            f[13][1] = f[22][3];
            f[22] = u.lq(47);
            f[13][2] = f[23][3];
            f[31] = u.lq(48);
            f[14][1] = f[25][0];
            f[14][2] = f[26][0];
            f[23] = Vec{{f[21][1], f[21][2], f[21][3], f[21][0]}};
            f[15][0] = f[24][1];
            f[15][2] = f[26][1];
            f[16][0] = f[24][2];
            f[16][1] = f[25][2];
            f[28] = Vec{{f[20][3], f[21][3], f[22][3], f[31][3]}};
            f[20][3] = f[31][0];
            f[21][3] = f[31][1];
            f[22][3] = f[31][2];
        }

        // 0xab0 (hdHatSphereTestAsm): a sphere (centre vf11, radius vf12.w) against the segment vf12 .. vf13: vi3 0x80
        // when the first end is inside it, or when the second is and the segment comes at it from the outside.
        void hatSphere(Vu &u)
        {
            Vec *f = u.vf;
            for (int k = 0; k < 3; ++k)
                f[14][k] = f[12][k] - f[11][k];
            f[16][0] = 1.0f;
            f[16][1] = 1.0f;
            u.vi[2] = 0x80;
            for (int k = 0; k < 3; ++k)
                f[15][k] = f[13][k] - f[11][k];
            f[12][3] = f[12][3] * f[12][3];
            for (int k = 0; k < 3; ++k)
                f[17][k] = f[14][k] * f[14][k];
            for (int k = 0; k < 3; ++k)
                f[20][k] = f[15][k] * f[15][k];
            f[10][0] = f[16][0] * f[12][3];
            u.acc[0] = f[14][0] * f[14][0] + f[16][0] * f[17][1] + f[16][0] * f[17][2];
            f[18][0] = u.acc[0] - f[16][0] * f[10][0];
            f[21][0] = u.acc[0];
            u.acc[0] = f[15][0] * f[15][0] + f[16][0] * f[20][1];
            f[20][0] = u.acc[0] + f[16][0] * f[20][2];
            u.vi[3] = negative(f[18][0]) ? 0x80 : 0;
            for (int k = 0; k < 3; ++k)
                f[22][k] = f[12][k] - f[13][k];
            f[23][0] = f[10][0] - f[21][0];
            if (u.vi[3] != 0)
                return;
            u.q = 1.0f / std::sqrt(std::fabs(f[20][0]));
            for (int k = 0; k < 3; ++k)
                f[17][k] = f[22][k] * f[22][k];
            u.acc[0] = f[22][0] * f[22][0] + f[16][0] * f[17][1] + f[16][0] * f[17][2];
            f[17][0] = u.acc[0] - f[16][0] * f[10][0];
            for (int k = 0; k < 3; ++k)
                f[24][k] = f[15][k] * u.q;
            u.vi[3] = negative(f[17][0]) ? 0x80 : 0;
            for (int k = 0; k < 3; ++k)
                f[25][k] = f[24][k] * f[14][k];
            for (int k = 0; k < 3; ++k)
                f[26][k] = f[24][k] * f[22][k];
            u.acc[0] = f[24][0] * f[14][0] + f[16][0] * f[25][1];
            f[27][0] = u.acc[0] + f[16][0] * f[25][2];
            u.vi[7] = 0x40;
            u.acc[1] = f[24][1] * f[22][1] + f[16][1] * f[26][0];
            f[28][1] = u.acc[1] + f[16][1] * f[26][2];
            u.acc[0] = f[23][0];
            f[23][0] = u.acc[0] + f[27][0] * f[27][0];
            u.vi[4] = negative(f[27][0]) ? 0x80 : 0;
            u.vi[5] = negative(f[28][1]) ? 0x40 : 0;
            if (u.vi[4] != 0)
            {
                u.vi[3] = 0;
                return;
            }
            u.vi[5] = negative(f[23][0]) ? 0x80 : 0;
            if (u.vi[5] != 0)
                u.vi[3] = 0;
        }

        bool isHierImage(const uint8_t *code)
        {
            static constexpr uint32_t kPairs[][3] = {{0x278u, 0x3f800000u, 0x800002ffu}, {0x808u, 0x100e0000u, 0x01e00498u},
                                                     {0x450u, 0x81f05b3cu, 0x01e0645bu}, {0xab0u, 0x8000033cu, 0x01cb63acu}};
            for (const auto &pair : kPairs)
            {
                uint32_t words[2];
                std::memcpy(words, code + pair[0], 8);
                if (words[0] != pair[1] || words[1] != pair[2])
                    return false;
            }
            return true;
        }

        bool run(R5900Context *ctx, const uint8_t *code, uint8_t *data, uint32_t pc)
        {
            void (*program)(Vu &) = nullptr;
            if (is2dImage(code))
            {
                switch (pc)
                {
                case 0x278: program = &setup2d; break;
                case 0x548: program = &stripFirst; break;
                case 0x5e0: program = &stripPoint; break;
                case 0x3b0: program = &lineFirst; break;
                case 0x438: program = &linePoint; break;
                default: break;
                }
            }
            else if (isHierImage(code))
            {
                switch (pc)
                {
                case 0x278: program = &fovTest; break;
                case 0x3a8: program = &scaledDistance; break;
                case 0x410: program = &rotatePoint; break;
                case 0x450: program = &cameraUpdate; break;
                case 0x5a8: program = &hierRoot; break;
                case 0x670: program = &hierRotate; break;
                case 0x808: program = &lights; break;
                case 0xab0: program = &hatSphere; break;
                default: break;
                }
            }
            if (!program)
                return false;
            Vu u(ctx, data);
            program(u);
            u.store(ctx);
            return true;
        }
    }

    void install()
    {
        const char *value = std::getenv("TMB_VU0_NATIVE");
        if (value && *value == '0')
            return;
        ps2x::native::setVu0Hle(&run);
    }
}
