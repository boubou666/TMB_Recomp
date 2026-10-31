// TMB's textures for the native renderer, from the native texture pack converted offline from the disc's files
// (tools/tmb_texture_pack.py, scripts/recomp.sh): nothing is read from the game's texture records, GS packets or GS
// memory while the game runs. The runtime only follows TMB's texture code, at hooks:
// - which disc file goes where: fileReadf / zipInflateAll (name, destination), then fileOnlyTexFile /
//   fileAddTexFile / fileOnlyResFile / fileAddResFile (destination) list the level's .TEX and .RTX files in order;
// - texmInit sets up each .TEX record in order (texmSetupTexture: its texture id, width, height, format): the n-th
//   setup is the pack's n-th record of those files, so a texture id names a pack record;
// - texmLoadResTexture loads each .RTX record in order (sceGsSetDefLoadImage: its VRAM block, format, size): a palette's or
//   an H texture's VRAM block names a pack record;
// - fontInit: the font's VRAM blocks (the pack's FONT file).
// A draw's TEX0 then finds its texture: TBP0 -> the texture id the game's own table (texInfo: TBP | ... | resident
// << 31 per id, word 1 of 4) has there, CBP -> the palette loaded at that block.
//
// Each hook checks the pack record against the game's own arguments (format, size) and reports a mismatch.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "tmb_region.h"
#include "runtime/native/native_texture_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef TMB_TEXTURE_PACK
#define TMB_TEXTURE_PACK "tmb_textures.pack"
#endif

namespace tmb::textures
{
    namespace
    {
        using Texture = ps2x::native::TextureCache::Texture;

        constexpr uint32_t kTexInfo = TMB_ADDR(0x004D4930u); // texInfo[1000], 16 bytes each
        constexpr uint32_t kFileReadf = TMB_ADDR(0x001C31D8u);
        constexpr uint32_t kZipInflateAll = TMB_ADDR(0x00201C30u);
        constexpr uint32_t kFileOnlyTexFile = TMB_ADDR(0x001C3B68u);
        constexpr uint32_t kFileAddTexFile = TMB_ADDR(0x001C3948u);
        constexpr uint32_t kFileOnlyResFile = TMB_ADDR(0x001C3BB0u);
        constexpr uint32_t kFileAddResFile = TMB_ADDR(0x001C39E0u);
        constexpr uint32_t kTexmInit = TMB_ADDR(0x001F9A78u);
        constexpr uint32_t kTexmSetupTexture = TMB_ADDR(0x001FA170u);
        constexpr uint32_t kTexmResInit = TMB_ADDR(0x001F9B08u);
        constexpr uint32_t kSceGsSetDefLoadImage = TMB_ADDR(0x00205140u);
        constexpr uint32_t kTexmLoadResTexture = TMB_ADDR(0x001F9BC8u);
        constexpr uint32_t kResLoadReturn = TMB_ADDR(0x001F9CC4u); // texmLoadResTexture's call of sceGsSetDefLoadImage
        constexpr uint32_t kFontInit = TMB_ADDR(0x001C4E50u);

        enum : uint32_t
        {
            CT32 = 0x00, CT24 = 0x01, CT16 = 0x02, CT16S = 0x0a, T8 = 0x13, T4 = 0x14, T8H = 0x1b, T4HL = 0x24, T4HH = 0x2c
        };
        bool isH(uint32_t psm) { return psm == T8H || psm == T4HL || psm == T4HH; }
        bool indexed(uint32_t psm) { return psm == T8 || psm == T4 || isH(psm); }

        struct Record
        {
            uint32_t psm = 0, width = 0, height = 0;
            std::vector<uint8_t> texels; // indexed: a byte per texel; CT32 / CT24: 4 bytes; CT16: 2 bytes
        };

        struct Store
        {
            std::mutex mutex;
            bool loaded = false;
            std::unordered_map<std::string, std::vector<Record>> pack; // file name -> its records in order
            std::unordered_map<uint32_t, std::string> nameAt;          // destination -> the disc file read there
            std::vector<std::string> texFiles, resFiles;               // the level's, in the game's order
            uint32_t texOrdinal = 0, resOrdinal = 0;
            std::unordered_map<uint32_t, const Record *> byId;    // texture id -> .TEX record
            std::unordered_map<uint32_t, const Record *> byBlock; // VRAM block -> .RTX record
            std::unordered_map<uint32_t, uint32_t> idAt;          // TBP -> texture id (texInfo, refresh)
            uint32_t fontTbp = ~0u;
            std::unordered_map<uint64_t, std::shared_ptr<const Texture>> decoded;
            uint64_t decodes = 0;
            const uint8_t *rdram = nullptr;
        };
        Store &store()
        {
            static Store s_store;
            return s_store;
        }

        void loadPack(Store &s)
        {
            if (s.loaded)
                return;
            s.loaded = true;
            const char *path = std::getenv("TMB_TEXTURE_PACK");
            if (!path || !*path)
                path = TMB_TEXTURE_PACK;
            std::ifstream in(path, std::ios::binary);
            char magic[8] = {};
            uint32_t files = 0;
            if (!in.read(magic, 8) || std::memcmp(magic, "TMBTEX1", 8) != 0 || !in.read(reinterpret_cast<char *>(&files), 4))
            {
                std::fprintf(stderr, "[tmb:tex] no texture pack at %s (tools/tmb_texture_pack.py): the native draws are untextured\n", path);
                return;
            }
            size_t records = 0;
            for (uint32_t f = 0; f < files; ++f)
            {
                uint16_t length = 0;
                in.read(reinterpret_cast<char *>(&length), 2);
                std::string name(length, '\0');
                in.read(name.data(), length);
                uint32_t count = 0;
                in.read(reinterpret_cast<char *>(&count), 4);
                std::vector<Record> &list = s.pack[name];
                list.resize(count);
                for (Record &r : list)
                {
                    uint32_t header[4];
                    in.read(reinterpret_cast<char *>(header), 16);
                    r.psm = header[0];
                    r.width = header[1];
                    r.height = header[2];
                    r.texels.resize(header[3]);
                    in.read(reinterpret_cast<char *>(r.texels.data()), header[3]);
                }
                records += count;
            }
            std::fprintf(stderr, "[tmb:tex] texture pack %s: %u files, %zu records\n", path, files, records);
        }

        // "cdrom0:\LVL\JUNKYARD.TEX;1" -> "JUNKYARD.TEX"
        std::string baseName(const uint8_t *rdram, uint32_t address)
        {
            std::string name;
            for (uint32_t i = 0; i < 256u; ++i)
            {
                const char c = static_cast<char>(rdram[(address + i) & 0x1ffffffu]);
                if (c == '\0' || c == ';')
                    break;
                if (c == '\\' || c == '/' || c == ':')
                    name.clear();
                else
                    name.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
            }
            return name;
        }

        // The n-th record of the listed files.
        const Record *nth(Store &s, const std::vector<std::string> &files, uint32_t n)
        {
            for (const std::string &file : files)
            {
                const auto it = s.pack.find(file);
                if (it == s.pack.end())
                    return nullptr;
                if (n < it->second.size())
                    return &it->second[n];
                n -= static_cast<uint32_t>(it->second.size());
            }
            return nullptr;
        }

        void mismatch(const char *what, uint32_t n, const Record *r, uint32_t psm, uint32_t width, uint32_t height)
        {
            static uint32_t s_reported = 0;
            if (s_reported++ < 20u)
                std::fprintf(stderr, "[tmb:tex] %s #%u: the game sets up psm %#x %ux%u, the pack has %s\n", what, n, psm, width,
                             height, r ? (std::to_string(r->psm) + " " + std::to_string(r->width) + "x" + std::to_string(r->height)).c_str() : "nothing");
        }

        void noteFile(uint8_t *rdram, R5900Context *ctx)
        {
            Store &s = store();
            std::lock_guard<std::mutex> lock(s.mutex);
            s.rdram = rdram;
            s.nameAt[getRegU32(ctx, 5)] = baseName(rdram, getRegU32(ctx, 4));
        }
        void fileReadfOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            noteFile(rdram, ctx);
            fileReadf__FPcPv_0x1c31d8(rdram, ctx, runtime);
        }
        void zipInflateAllOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            noteFile(rdram, ctx);
            zipInflateAll__FPcPv_0x201c30(rdram, ctx, runtime);
        }

        void listFile(R5900Context *ctx, bool tex, bool only)
        {
            Store &s = store();
            std::lock_guard<std::mutex> lock(s.mutex);
            std::vector<std::string> &files = tex ? s.texFiles : s.resFiles;
            if (only)
                files.clear();
            const auto it = s.nameAt.find(getRegU32(ctx, 4));
            files.push_back(it != s.nameAt.end() ? it->second : std::string("?"));
        }
        void fileOnlyTexFileOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            listFile(ctx, true, true);
            fileOnlyTexFile__FPci_0x1c3b68(rdram, ctx, runtime);
        }
        void fileAddTexFileOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            listFile(ctx, true, false);
            fileAddTexFile__FPci_0x1c3948(rdram, ctx, runtime);
        }
        void fileOnlyResFileOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            listFile(ctx, false, true);
            fileOnlyResFile__FPci_0x1c3bb0(rdram, ctx, runtime);
        }
        void fileAddResFileOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            listFile(ctx, false, false);
            fileAddResFile__FPci_0x1c39e0(rdram, ctx, runtime);
        }

        void texmInitOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                Store &s = store();
                std::lock_guard<std::mutex> lock(s.mutex);
                loadPack(s);
                s.rdram = rdram;
                s.texOrdinal = 0;
                s.byId.clear();
                s.decoded.clear();
            }
            texmInit__F10_vramAddrs_0x1f9a78(rdram, ctx, runtime);
            Store &s = store();
            std::lock_guard<std::mutex> lock(s.mutex);
            std::string files;
            for (const std::string &f : s.texFiles)
                files += " " + f;
            std::fprintf(stderr, "[tmb:tex] texmInit: %u textures set up from%s\n", s.texOrdinal, files.c_str());
        }

        // texmSetupTexture(id, width, height, psm, ...): once per .TEX record, in order.
        void texmSetupTextureOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                Store &s = store();
                std::lock_guard<std::mutex> lock(s.mutex);
                const uint32_t id = getRegU32(ctx, 4) & 0xffffu, width = getRegU32(ctx, 5), height = getRegU32(ctx, 6),
                               psm = getRegU32(ctx, 7) & 0x3fu;
                const uint32_t n = s.texOrdinal++;
                const Record *r = nth(s, s.texFiles, n);
                if (n == 0u)
                    std::fprintf(stderr, "[tmb:tex] texmSetupTexture: the first of %zu files (id %u, psm %#x %ux%u)\n", s.texFiles.size(), id, psm,
                                 width, height);
                if (r && r->psm == psm && r->width == width && r->height == height)
                    s.byId[id] = r;
                else
                    mismatch("texture", n, r, psm, width, height);
            }
            texmSetupTexture__FiiiiiP6QwDataii_0x1fa170(rdram, ctx, runtime);
        }

        void texmResInitOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                Store &s = store();
                std::lock_guard<std::mutex> lock(s.mutex);
                loadPack(s);
                s.rdram = rdram;
                s.resOrdinal = 0;
                s.byBlock.clear();
                s.decoded.clear();
            }
            texmResInit__F10_vramAddrsP6QwData_0x1f9b08(rdram, ctx, runtime);
            Store &s = store();
            std::lock_guard<std::mutex> lock(s.mutex);
            std::string files;
            for (const std::string &f : s.resFiles)
                files += " " + f;
            std::fprintf(stderr, "[tmb:tex] texmResInit: %u palettes and resident textures loaded from%s\n", s.resOrdinal, files.c_str());
        }

        // The shell calls it itself when texmResInit's check (texmResCheckForData) found nothing to load yet; texmResInit
        // tail-jumps into it (a direct call: its records are counted from texmResInit's override).
        void texmLoadResTextureOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                Store &s = store();
                std::lock_guard<std::mutex> lock(s.mutex);
                loadPack(s);
                s.rdram = rdram;
                s.resOrdinal = 0;
                s.byBlock.clear();
                s.decoded.clear();
            }
            texmLoadResTexture__FP6QwData_0x1f9bc8(rdram, ctx, runtime);
            Store &s = store();
            std::lock_guard<std::mutex> lock(s.mutex);
            std::fprintf(stderr, "[tmb:tex] texmLoadResTexture: %u palettes and resident textures loaded\n", s.resOrdinal);
        }

        // sceGsSetDefLoadImage(image, dbp, dbw, dpsm, x, y, w, h), called by texmLoadResTexture (its return address)
        // once per .RTX record, in order.
        void sceGsSetDefLoadImageOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                Store &s = store();
                std::lock_guard<std::mutex> lock(s.mutex);
                if (getRegU32(ctx, 31) == kResLoadReturn)
                {
                    const uint32_t block = getRegU32(ctx, 5) & 0x3fffu, psm = getRegU32(ctx, 7) & 0x3fu;
                    const uint32_t width = getRegU32(ctx, 10) & 0xffffu, height = getRegU32(ctx, 11) & 0xffffu;
                    const uint32_t n = s.resOrdinal++;
                    const Record *r = nth(s, s.resFiles, n);
                    if (r && r->psm == psm && r->width == width && r->height == height)
                        s.byBlock[block] = r;
                    else
                        mismatch("resident record", n, r, psm, width, height);
                }
            }
            sceGsSetDefLoadImage_0x205140(rdram, ctx, runtime);
        }

        void fontInitOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            {
                Store &s = store();
                std::lock_guard<std::mutex> lock(s.mutex);
                loadPack(s);
                s.fontTbp = static_cast<uint32_t>((static_cast<int32_t>(getRegU32(ctx, 4)) - 0x840) / 64);
            }
            fontInit__F10_vramAddrs_0x1c4e50(rdram, ctx, runtime);
        }

        uint32_t fromCt16(uint32_t c, uint64_t texa)
        {
            const uint32_t r = (c & 0x1fu) << 3, g = ((c >> 5) & 0x1fu) << 3, b = ((c >> 10) & 0x1fu) << 3;
            const uint32_t ta0 = texa & 0xffu, ta1 = (texa >> 32) & 0xffu;
            const bool aem = (texa >> 15) & 1u;
            const uint32_t a = (c & 0x8000u) ? ta1 : ((aem && (c & 0x7fffu) == 0u) ? 0u : ta0);
            return r | (g << 8) | (b << 16) | (a << 24);
        }
        // A record's entry as RGBA with the GS's alpha (0x80 is 1.0), as ps2x::native::TextureCache gives it.
        uint32_t colour(const Record &r, uint32_t index, uint64_t texa)
        {
            if (r.psm == CT16 || r.psm == CT16S)
            {
                uint16_t c;
                std::memcpy(&c, r.texels.data() + index * 2u, 2);
                return fromCt16(c, texa);
            }
            uint32_t c;
            std::memcpy(&c, r.texels.data() + index * 4u, 4);
            if (r.psm == CT24)
            {
                const bool aem = (texa >> 15) & 1u;
                const uint32_t a = (aem && (c & 0xffffffu) == 0u) ? 0u : static_cast<uint32_t>(texa & 0xffu);
                return (c & 0xffffffu) | (a << 24);
            }
            return c;
        }
    }

    // TBP -> texture id, from the game's table as it left it after building the frame the adapter reads (called at
    // each chain the adapter reads, before its draws).
    void refresh(const uint8_t *rdram)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (rdram)
            s.rdram = rdram;
        if (!s.rdram)
            return;
        s.idAt.clear();
        for (uint32_t id = 0; id < 1000u; ++id)
        {
            uint32_t info;
            std::memcpy(&info, s.rdram + kTexInfo + id * 16u + 4u, 4);
            if ((info >> 31) != 0u)
                s.idAt[info & 0x3fffu] = id;
        }
    }

    // The texture a draw's TEX0 shows (null when the game set up none there: reported once).
    std::shared_ptr<const Texture> lookup(uint64_t tex0, uint64_t texa)
    {
        Store &s = store();
        std::lock_guard<std::mutex> lock(s.mutex);
        const uint32_t tbp = tex0 & 0x3fffu, psm = (tex0 >> 20) & 0x3fu;
        const uint32_t cbp = (tex0 >> 37) & 0x3fffu, csa = (tex0 >> 56) & 0x1fu;
        const uint32_t width = 1u << std::min<uint32_t>(10u, (tex0 >> 26) & 0xfu);
        const uint32_t height = 1u << std::min<uint32_t>(10u, (tex0 >> 30) & 0xfu);

        const Record *texels = nullptr, *palette = nullptr;
        if (tbp == s.fontTbp && s.pack.count("FONT"))
        {
            texels = &s.pack["FONT"][0];
            palette = &s.pack["FONT"][1];
        }
        else if (isH(psm))
        {
            const auto it = s.byBlock.find(tbp);
            texels = it != s.byBlock.end() ? it->second : nullptr;
        }
        else if (const auto at = s.idAt.find(tbp); at != s.idAt.end())
        {
            const auto it = s.byId.find(at->second);
            texels = it != s.byId.end() && it->second->psm == psm ? it->second : nullptr;
        }
        if (texels && indexed(psm) && !palette)
        {
            const auto it = s.byBlock.find(cbp);
            palette = it != s.byBlock.end() ? it->second : nullptr;
        }
        if (!texels || (indexed(psm) && !palette))
        {
            static std::unordered_map<uint64_t, bool> s_reported;
            const uint64_t reportKey = tex0 & ~(uint64_t{7} << 61);
            if (!s_reported[reportKey])
            {
                s_reported[reportKey] = true;
                std::fprintf(stderr, "[tmb:tex] TEX0 %#llx (tbp %u psm %#x %ux%u cbp %u): no %s set up there\n",
                             static_cast<unsigned long long>(tex0), tbp, psm, width, height, cbp, texels ? "palette" : "texture");
            }
            return nullptr;
        }

        const uint64_t key = (reinterpret_cast<uintptr_t>(texels) * 0x9e3779b97f4a7c15ull) ^
                             (reinterpret_cast<uintptr_t>(palette) * 0xc2b2ae3d27d4eb4full) ^
                             (tex0 & ~((uint64_t{3} << 35) | (uint64_t{7} << 61))) ^ (texa * 0x165667b19e3779f9ull);
        if (const auto found = s.decoded.find(key); found != s.decoded.end())
            return found->second;

        uint32_t colours[256] = {};
        if (indexed(psm))
        {
            const bool eight = psm == T8 || psm == T8H;
            for (uint32_t i = 0; i < (eight ? 256u : 16u); ++i)
            {
                uint32_t px = 0, py = 0;
                ps2x::native::csm1Position(eight ? i : (csa * 16u + i) & 0xffu, px, py);
                if (px < palette->width && py < palette->height)
                    colours[i] = colour(*palette, py * palette->width + px, texa);
            }
        }

        Texture texture;
        texture.width = width;
        texture.height = height;
        texture.key = key;
        texture.version = ++s.decodes;
        texture.rgba.resize(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t c = 0;
                if (x < texels->width && y < texels->height)
                {
                    const uint32_t at = y * texels->width + x;
                    c = indexed(psm) ? colours[texels->texels[at]] : colour(*texels, at, texa);
                }
                texture.rgba[static_cast<size_t>(y) * width + x] = c;
            }
        auto shared = std::make_shared<const Texture>(std::move(texture));
        s.decoded[key] = shared;
        return shared;
    }

    void install(PS2Runtime &runtime)
    {
        runtime.registerFunction(kFileReadf, &fileReadfOverride);
        runtime.registerFunction(kZipInflateAll, &zipInflateAllOverride);
        runtime.registerFunction(kFileOnlyTexFile, &fileOnlyTexFileOverride);
        runtime.registerFunction(kFileAddTexFile, &fileAddTexFileOverride);
        runtime.registerFunction(kFileOnlyResFile, &fileOnlyResFileOverride);
        runtime.registerFunction(kFileAddResFile, &fileAddResFileOverride);
        runtime.registerFunction(kTexmInit, &texmInitOverride);
        runtime.registerFunction(kTexmSetupTexture, &texmSetupTextureOverride);
        runtime.registerFunction(kTexmResInit, &texmResInitOverride);
        runtime.registerFunction(kTexmLoadResTexture, &texmLoadResTextureOverride);
        runtime.registerFunction(kSceGsSetDefLoadImage, &sceGsSetDefLoadImageOverride);
        runtime.registerFunction(kFontInit, &fontInitOverride);
    }
}
