#include "enhanced/world_decode.h"

#include <cstddef>
#include <cstdio>

namespace vette::enhanced {

namespace {
constexpr int kMultiples[8] = {1, 2, 3, 5, 7, 8, 9, 11};
}

AxisTable ideal_axis_table() {
    AxisTable t;
    // Axis j's unit vector in world component order (x north, y east, z up).
    const V3s axes[3] = {{0, 1024, 0}, {0, 0, 1024}, {1024, 0, 0}};
    for (size_t a = 0; a < 3; ++a) {
        for (size_t k = 0; k < 8; ++k) {
            V3s v{};
            for (size_t c = 0; c < 3; ++c) {
                v[c] = static_cast<int16_t>(axes[a][c] * kMultiples[k]);
            }
            t.vectors[a * 8 + k] = v;
        }
    }
    return t;
}

bool unpack_vertices(const V3s& origin, const int16_t* words, size_t count, const AxisTable& table,
                     std::vector<V3s>& out) {
    out.clear();
    out.reserve(count + 1);
    out.push_back(origin);
    V3s cur = origin;
    for (size_t i = 0; i < count; ++i) {
        uint16_t acc[3] = {0, 0, 0};
        for (size_t j = 0; j < 3; ++j) {
            const auto w = static_cast<uint16_t>(words[i * 3 + j]);
            if (w == 0) {
                continue;
            }
            const auto lo = static_cast<int8_t>(w & 0xFF);
            const auto shift = static_cast<unsigned>(w >> 8);
            // NEG AL / JG / NEG AL / CBW: |lo|, except that -128 stays -128.
            const int mag = (lo < 0 && lo != -128) ? -lo : lo;
            const int offset = table.base[j] + mag - 6;  // relative to DS:3184
            if (offset < 0 || offset % 6 != 0 || offset / 6 >= 24) {
                return false;
            }
            V3s v = table.vectors[static_cast<size_t>(offset / 6)];
            for (int16_t& c : v) {
                if (lo < 0) {
                    c = static_cast<int16_t>(static_cast<uint16_t>(-c));
                }
                c = sar_round(c, shift);
                (void)c;
            }
            for (size_t c = 0; c < 3; ++c) {
                acc[c] = static_cast<uint16_t>(acc[c] + static_cast<uint16_t>(v[c]));
            }
        }
        for (size_t c = 0; c < 3; ++c) {
            cur[c] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint16_t>(cur[c]) + acc[c]));
        }
        out.push_back(cur);
    }
    return true;
}

namespace {

bool read_face(const ImageView& img, uint16_t seg, uint16_t at, ModelFace& f, std::string& error) {
    f.address = at;
    f.flags = img.u16(seg, at);
    const uint16_t colour = img.u16(seg, static_cast<uint16_t>(at + 2));
    f.colour.raw = static_cast<uint8_t>(colour);
    f.colour_hi = static_cast<uint8_t>(colour >> 8);
    uint16_t p = static_cast<uint16_t>(at + 4);
    for (int guard = 0;; ++guard) {
        const uint16_t n = img.u16(seg, p);
        if (n == 0xFFFF) {
            return true;
        }
        if (guard >= 64 || n == 0 || n > 64) {
            char buf[80];
            std::snprintf(buf, sizeof buf, "model face %04X: bad primitive at %04X", at, p);
            error = buf;
            return false;
        }
        std::vector<uint16_t> idx;
        for (uint16_t i = 0; i <= n; ++i) {
            idx.push_back(static_cast<uint16_t>(img.u16(seg, static_cast<uint16_t>(p + 2 + 2 * i)) / 4));
        }
        if (!f.lines()) {
            idx.pop_back();  // fills: the closing repeat of the first index
        }
        f.prims.push_back(std::move(idx));
        p = static_cast<uint16_t>(p + 2 + 2 * (n + 1));
    }
}

} // namespace

bool decode_model_mesh(const ImageView& img, uint16_t seg, uint16_t header, ModelMesh& out, std::string& error,
                       int colour_override, const std::vector<uint16_t>& patched_faces) {
    out = ModelMesh{};
    out.header = header;
    const uint16_t hseg = img.u16(seg, header);
    const uint16_t nverts = img.u16(seg, static_cast<uint16_t>(header + 2));
    const uint16_t vptr = img.u16(seg, static_cast<uint16_t>(header + 4));
    const uint16_t optr = img.u16(seg, static_cast<uint16_t>(header + 6));
    char buf[96];
    // The header's own segment holds the model (245A for the original's; an edited one elsewhere, as
    // game/model_pack.h writes them).
    if (hseg == 0 || nverts < 4 || nverts > 128) {
        std::snprintf(buf, sizeof buf, "model header %04X: segment %04X, %u vertices", header, hseg, nverts);
        error = buf;
        return false;
    }
    const uint16_t header_seg = seg;
    seg = hseg;
    for (uint16_t i = 0; i < nverts; ++i) {
        const auto at = static_cast<uint16_t>(vptr + 6 * i);
        out.verts.push_back({img.s16(seg, at), img.s16(seg, static_cast<uint16_t>(at + 2)),
                             img.s16(seg, static_cast<uint16_t>(at + 4))});
    }
    std::vector<uint16_t> face_addr;
    for (size_t o = 0; o < 8; ++o) {
        uint16_t p = img.u16(seg, static_cast<uint16_t>(optr + 2 * o));
        // Skip list: countdown values (vertex i has countdown nverts - i), ended by a negative word.
        for (int guard = 0;; ++guard, p = static_cast<uint16_t>(p + 2)) {
            const int16_t v = img.s16(seg, p);
            if (v < 0) {
                break;
            }
            if (guard > 256) {
                error = "model skip list unterminated";
                return false;
            }
            out.skipped[o].push_back(static_cast<uint16_t>(nverts - v));
        }
        p = static_cast<uint16_t>(p + 2);
        for (int guard = 0;; ++guard, p = static_cast<uint16_t>(p + 2)) {
            const uint16_t fa = img.u16(seg, p);
            if (fa == 0xFFFF) {
                break;
            }
            if (guard > 256) {
                error = "model face list unterminated";
                return false;
            }
            size_t index = 0;
            while (index < face_addr.size() && face_addr[index] != fa) {
                ++index;
            }
            if (index == face_addr.size()) {
                ModelFace f;
                if (!read_face(img, seg, fa, f, error)) {
                    return false;
                }
                if (colour_override >= 0 && seg == header_seg) {  // (the far boxes: the original's)
                    for (const uint16_t pf : patched_faces) {
                        if (static_cast<uint16_t>(fa + 2) == pf) {
                            f.colour.raw = static_cast<uint8_t>(colour_override);
                            f.colour_hi = static_cast<uint8_t>(colour_override >> 8);
                        }
                    }
                }
                for (const auto& prim : f.prims) {
                    for (const uint16_t v : prim) {
                        if (v >= nverts) {
                            std::snprintf(buf, sizeof buf, "model face %04X: vertex %u of %u", fa, v, nverts);
                            error = buf;
                            return false;
                        }
                    }
                }
                face_addr.push_back(fa);
                out.faces.push_back(std::move(f));
            }
            out.order[o].push_back(static_cast<uint16_t>(index));
        }
    }
    return true;
}

uint16_t read_entry_list(const ImageView& img, uint16_t ds, uint16_t at, std::vector<ListEntry>& out,
                         size_t max_entries) {
    out.clear();
    for (size_t i = 0; i <= max_entries; ++i) {
        const uint16_t code = img.u16(ds, at);
        if (code == 0xFFFF) {
            return static_cast<uint16_t>(at + 2);
        }
        out.push_back({code, img.s16(ds, static_cast<uint16_t>(at + 2)), img.s16(ds, static_cast<uint16_t>(at + 4)),
                       img.s16(ds, static_cast<uint16_t>(at + 6))});
        at = static_cast<uint16_t>(at + 8);
    }
    return 0;
}

} // namespace vette::enhanced
