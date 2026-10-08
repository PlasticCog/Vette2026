#include "game/model_pack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>

#include "game/x86.h"
#include "host/machine.h"

namespace vette::game {
namespace {

using host::Cpu;
using host::Memory;

constexpr uint16_t kModelSeg = emu_seg(0x245A), kModelTable = 0x6FF8;  // {w near, w mid, w far, w far colour}
constexpr uint16_t kTableEnd = kModelTable + 8 * kModelCount;  // the Chinatown gate's vertices follow
constexpr uint16_t kDrawUnrotated = 0xB9E6;  // draw model AX at the object's place (DS:3220), unrotated
constexpr uint16_t kObjectFarColour = 7;     // (the far box's colour patch, BA18: unused, all slots are near)
constexpr uint16_t kPackSeg = 0x8000;  // free conventional memory: the game keeps below 6E16h
constexpr uint16_t kEntrySeg = emu_seg(0x3009), kEntry = 0x0025;
constexpr const char* kMagic = "VETTE2026 MODELS 1";
constexpr ModelData::Vertex kReference[kReferenceVertices] = {{0, 0, 0}, {-50, 0, 0}, {0, 0, 50}, {0, 50, 0}};

// The original's own names where known (notes 05 section 6: matched to the Mac version's OBJS names).
const char* const kNames[kModelCount] = {
    "Taxi", "Corvette (yours)", "Cable car", "Car", "Fire truck", "Ambulance", "Porsche", "Police car", "Bus",
    "Lamborghini", "Ferrari F40", "Motorbike", "Testarossa", "Truck", "Chinatown gate", "Block 1", "Block 2",
    "Block 3", "Windmill", "Block 4", "Coit Tower", nullptr, "Pedestrian", nullptr, "Zoo", "Pier", "Ferry Building",
    "Pier 39", nullptr, "Pedestrian crossing", "Pedestrians", "Jogger", "Juggler", "Nun", "Lawyer", "Blind man",
    "Ghirardelli", "Barrier", "Fisherman's Wharf", "Hyatt", nullptr, "Holiday Inn", nullptr, "Doda", "Fairmont",
    "St Mary's", "Bank of America", "St Peter", "Goddess", nullptr, nullptr, "Wall", nullptr, nullptr, "Japantown",
    "Palace of Fine Arts", "Holiday park", nullptr, "Barrier"};

uint16_t near_header(Memory& m, int id) { return rd16(m, kModelSeg, static_cast<uint16_t>(kModelTable + 8 * id)); }

bool read_face(Memory& m, uint16_t seg, uint16_t at, uint16_t nverts, ModelData::Face& f) {
    f.flags = rd16(m, seg, at);
    const uint16_t colour = rd16(m, seg, static_cast<uint16_t>(at + 2));
    f.colour = static_cast<uint8_t>(colour);
    f.outline = static_cast<uint8_t>(colour >> 8);
    uint16_t p = static_cast<uint16_t>(at + 4);
    for (int guard = 0;; ++guard) {
        const uint16_t n = rd16(m, seg, p);
        if (n == 0xFFFF) return !f.prims.empty();
        if (guard >= 64 || n == 0 || n > 64) return false;
        std::vector<uint16_t> idx;
        for (uint16_t i = 0; i <= n; ++i) {
            const uint16_t v = static_cast<uint16_t>(rd16(m, seg, static_cast<uint16_t>(p + 2 + 2 * i)) / 4);
            if (v >= nverts) return false;
            idx.push_back(v);
        }
        if (!f.lines()) idx.pop_back();  // fills repeat their first point
        f.prims.push_back(std::move(idx));
        p = static_cast<uint16_t>(p + 2 + 2 * (n + 1));
    }
}

std::string hex(unsigned v, int digits) {
    char buf[12];
    std::snprintf(buf, sizeof buf, "%0*X", digits, v);
    return buf;
}

}  // namespace

std::string model_name(int id) {
    if (id >= 0 && id < kModelCount && kNames[id]) return kNames[id];
    return "Object " + std::to_string(id);
}

std::optional<ModelData> read_model(Memory& m, int id) {
    if (id < 0 || id >= kModelCount) return std::nullopt;
    const uint16_t header = near_header(m, id);
    if (header == 0 || header == 0xFFFF) return std::nullopt;
    const uint16_t seg = rd16(m, kModelSeg, header), n = rd16(m, kModelSeg, static_cast<uint16_t>(header + 2));
    const uint16_t vptr = rd16(m, kModelSeg, static_cast<uint16_t>(header + 4));
    const uint16_t optr = rd16(m, kModelSeg, static_cast<uint16_t>(header + 6));
    if (seg == 0 || n < kReferenceVertices || n > kMaxVertices) return std::nullopt;
    ModelData d;
    for (uint16_t i = 0; i < n; ++i) {
        const auto at = static_cast<uint16_t>(vptr + 6 * i);
        d.verts.push_back({static_cast<int16_t>(rd16(m, seg, at)), static_cast<int16_t>(rd16(m, seg, static_cast<uint16_t>(at + 2))),
                           static_cast<int16_t>(rd16(m, seg, static_cast<uint16_t>(at + 4)))});
    }
    std::vector<uint16_t> addresses;
    for (int o = 0; o < 8; ++o) {
        uint16_t p = rd16(m, seg, static_cast<uint16_t>(optr + 2 * o));
        for (int guard = 0; static_cast<int16_t>(rd16(m, seg, p)) >= 0; ++guard, p = static_cast<uint16_t>(p + 2)) {
            if (guard > 256) return std::nullopt;  // (the skip list: rebuilt when written)
        }
        p = static_cast<uint16_t>(p + 2);
        for (int guard = 0;; ++guard, p = static_cast<uint16_t>(p + 2)) {
            const uint16_t fa = rd16(m, seg, p);
            if (fa == 0xFFFF) break;
            if (guard > 256) return std::nullopt;
            const auto it = std::find(addresses.begin(), addresses.end(), fa);
            if (it == addresses.end()) {
                ModelData::Face f;
                if (!read_face(m, seg, fa, n, f)) return std::nullopt;
                addresses.push_back(fa);
                d.faces.push_back(std::move(f));
                d.order[static_cast<size_t>(o)].push_back(static_cast<uint16_t>(d.faces.size() - 1));
            } else {
                d.order[static_cast<size_t>(o)].push_back(static_cast<uint16_t>(it - addresses.begin()));
            }
        }
    }
    return d;
}

namespace {

// Whether face `decal` lies in the plane of polygon face `base` (all its points within 2 units of it) and
// isn't `base` itself: drawn on it.
bool painted_on(const ModelData& model, uint16_t decal, uint16_t base) {
    if (decal == base) return false;
    const ModelData::Face& b = model.faces[base];
    if (b.lines() || b.prims.empty() || b.prims.front().size() < 3) return false;
    const auto& p = b.prims.front();
    const auto at = [&](uint16_t v) {
        const auto& q = model.verts[v];
        return std::array<double, 3>{double(q[0]), double(q[1]), double(q[2])};
    };
    // Newell's normal of the base.
    std::array<double, 3> n{0, 0, 0};
    for (size_t i = 0; i < p.size(); ++i) {
        const auto c = at(p[i]), d = at(p[(i + 1) % p.size()]);
        n[0] += (c[1] - d[1]) * (c[2] + d[2]);
        n[1] += (c[2] - d[2]) * (c[0] + d[0]);
        n[2] += (c[0] - d[0]) * (c[1] + d[1]);
    }
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-9) return false;
    const auto o = at(p.front());
    // ...and within its bounds (two faces in one plane: the smaller is on the larger, not the other way).
    std::array<double, 3> lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    for (const uint16_t v : p) {
        const auto q = at(v);
        for (size_t c = 0; c < 3; ++c) lo[c] = std::min(lo[c], q[c] - 2), hi[c] = std::max(hi[c], q[c] + 2);
    }
    bool any = false;
    for (const auto& prim : model.faces[decal].prims) {
        for (const uint16_t v : prim) {
            if (v >= model.verts.size()) return false;
            const auto q = at(v);
            if (std::fabs((n[0] * (q[0] - o[0]) + n[1] * (q[1] - o[1]) + n[2] * (q[2] - o[2])) / len) > 2) return false;
            for (size_t c = 0; c < 3; ++c) {
                if (q[c] < lo[c] || q[c] > hi[c]) return false;
            }
            any = true;
        }
    }
    return any;
}

}  // namespace

void make_orders(ModelData& model) {
    // A face's place: the middle of its points.
    std::vector<std::array<double, 3>> centre(model.faces.size());
    for (size_t i = 0; i < model.faces.size(); ++i) {
        double s[3] = {0, 0, 0};
        int n = 0;
        for (const auto& prim : model.faces[i].prims) {
            for (const uint16_t v : prim) {
                if (v >= model.verts.size()) continue;
                for (int c = 0; c < 3; ++c) s[c] += model.verts[v][static_cast<size_t>(c)];
                ++n;
            }
        }
        for (int c = 0; c < 3; ++c) centre[i][static_cast<size_t>(c)] = n ? s[c] / n : 0;
    }
    for (int o = 0; o < 8; ++o) {
        // The viewpoint: well out along the octant's diagonal (model_octant: bit 0 west, 1 north, 2 below).
        const double cam[3] = {(o & 1) ? -10000.0 : 10000.0, (o & 4) ? 10000.0 : -10000.0, (o & 2) ? 10000.0 : -10000.0};
        std::vector<std::pair<double, uint16_t>> by_distance;
        for (size_t i = 0; i < model.faces.size(); ++i) {
            if (model.faces[i].flags & 0x4000) continue;
            const double dx = cam[0] - centre[i][0], dy = cam[1] - centre[i][1], dz = cam[2] - centre[i][2];
            by_distance.emplace_back(-std::sqrt(dx * dx + dy * dy + dz * dz), static_cast<uint16_t>(i));
        }
        std::stable_sort(by_distance.begin(), by_distance.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        auto& order = model.order[static_cast<size_t>(o)];
        order.clear();
        for (const auto& [d, i] : by_distance) order.push_back(i);
        // A face painted on another goes after it (a window on its wall: without a depth buffer, the
        // later one shows).
        for (int pass = 0; pass < 4; ++pass) {
            bool moved = false;
            for (size_t a = 0; a < order.size(); ++a) {
                for (size_t b = 0; b < a; ++b) {
                    if (!painted_on(model, order[b], order[a])) continue;
                    const uint16_t f = order[b];
                    order.erase(order.begin() + static_cast<std::ptrdiff_t>(b));
                    order.insert(order.begin() + static_cast<std::ptrdiff_t>(a), f);
                    moved = true;
                    break;
                }
            }
            if (!moved) break;
        }
    }
}

std::string check_model(const ModelData& model) {
    if (model.verts.size() < kReferenceVertices) return "it has no vertices";
    if (model.verts.size() > kMaxVertices)
        return std::to_string(model.verts.size() - kReferenceVertices) + " vertices: the game takes " +
               std::to_string(kMaxVertices - kReferenceVertices) + " at most";
    for (int i = 0; i < kReferenceVertices; ++i) {
        if (model.verts[static_cast<size_t>(i)] != kReference[i]) return "its reference frame (vertices 0-3) moved";
    }
    for (size_t f = 0; f < model.faces.size(); ++f) {
        const ModelData::Face& face = model.faces[f];
        if (face.prims.empty()) return "face " + std::to_string(f) + " has nothing in it";
        for (const auto& prim : face.prims) {
            const size_t least = face.lines() ? 2 : 1;  // (a few of the original's fills are a point or two)
            if (prim.size() < least) return "face " + std::to_string(f) + " has too few points";
            if (prim.size() > kMaxPoints) return "face " + std::to_string(f) + " has more than " + std::to_string(kMaxPoints) + " points";
            for (const uint16_t v : prim) {
                if (v < kReferenceVertices || v >= model.verts.size()) return "face " + std::to_string(f) + " uses a vertex that isn't there";
            }
        }
    }
    for (const auto& order : model.order) {
        for (const uint16_t f : order) {
            if (f >= model.faces.size()) return "a view order lists a face that isn't there";
        }
    }
    return {};
}

std::string ModelPack::serialize() const {
    std::ostringstream out;
    out << kMagic << "\n";
    out << "# Models of VETTE! 2026's object editor. Vertices: x east, y down, z north; 0-3 are the model's\n"
           "# reference frame. A face: flags, colour, outline colour (hex), then its polygons' or lines'\n"
           "# vertices, '|' between them. An order: the faces drawn, back to front, seen from one octant.\n";
    const auto body = [&out](const ModelData& m) {
        for (const auto& v : m.verts) out << "v " << v[0] << " " << v[1] << " " << v[2] << "\n";
        for (const auto& f : m.faces) {
            out << "f " << hex(f.flags, 4) << " " << hex(f.colour, 2) << " " << hex(f.outline, 2);
            for (size_t p = 0; p < f.prims.size(); ++p) {
                if (p) out << " |";
                for (const uint16_t i : f.prims[p]) out << " " << i;
            }
            out << "\n";
        }
        for (size_t o = 0; o < m.order.size(); ++o) {
            if (m.order[o].empty()) continue;
            out << "o " << o;
            for (const uint16_t f : m.order[o]) out << " " << f;
            out << "\n";
        }
        out << "end\n";
    };
    for (const auto& [id, m] : models) {
        out << "model " << id << "  # " << model_name(id) << "\n";
        body(m);
    }
    for (const auto& [routine, m] : objects) {
        out << "object " << hex(routine, 4) << "  # the code-drawn object 3009:" << hex(routine, 4) << "\n";
        body(m);
    }
    return out.str();
}

std::optional<ModelPack> ModelPack::parse(const std::string& text, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int line_no = 0;
    const auto fail = [&](const std::string& what) {
        error = "line " + std::to_string(line_no) + ": " + what;
        return std::nullopt;
    };
    if (!std::getline(in, line) || line.rfind(kMagic, 0) != 0) {
        error = "not a VETTE! 2026 models file";
        return std::nullopt;
    }
    ++line_no;
    ModelPack pack;
    ModelData* model = nullptr;
    while (std::getline(in, line)) {
        ++line_no;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        std::string what;
        if (!(words >> what)) continue;
        if (what == "model") {
            int id = -1;
            if (!(words >> id) || id < 0 || id >= kModelCount) return fail("a model number from 0 to " + std::to_string(kModelCount - 1));
            model = &pack.models[id];
            *model = ModelData{};
        } else if (what == "object") {
            std::string at;
            unsigned long routine = 0;
            try {
                if (!(words >> at)) throw 0;
                routine = std::stoul(at, nullptr, 16);
                if (routine == 0 || routine > 0xFFFF) throw 0;
            } catch (...) {
                return fail("an object's routine is a hexadecimal address");
            }
            model = &pack.objects[static_cast<uint16_t>(routine)];
            *model = ModelData{};
            if (pack.objects.size() > kMaxObjects) return fail("more than " + std::to_string(kMaxObjects) + " objects");
        } else if (what == "end") {
            model = nullptr;
        } else if (!model) {
            return fail("\"" + what + "\" outside a model");
        } else if (what == "v") {
            long x = 0, y = 0, z = 0;
            if (!(words >> x >> y >> z) || std::abs(x) > 32767 || std::abs(y) > 32767 || std::abs(z) > 32767)
                return fail("a vertex is three whole numbers (-32767 to 32767)");
            model->verts.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z)});
        } else if (what == "f") {
            std::string flags, colour, outline;
            if (!(words >> flags >> colour >> outline)) return fail("a face starts with its flags, colour and outline");
            ModelData::Face f;
            try {
                f.flags = static_cast<uint16_t>(std::stoul(flags, nullptr, 16));
                f.colour = static_cast<uint8_t>(std::stoul(colour, nullptr, 16));
                f.outline = static_cast<uint8_t>(std::stoul(outline, nullptr, 16));
            } catch (...) {
                return fail("a face's flags and colours are hexadecimal");
            }
            f.prims.emplace_back();
            std::string word;
            while (words >> word) {
                if (word == "|") {
                    f.prims.emplace_back();
                    continue;
                }
                try {
                    const unsigned long v = std::stoul(word);
                    if (v > 0xFFFF) throw 0;
                    f.prims.back().push_back(static_cast<uint16_t>(v));
                } catch (...) {
                    return fail("\"" + word + "\" isn't a vertex number");
                }
            }
            model->faces.push_back(std::move(f));
        } else if (what == "o") {
            int o = -1;
            if (!(words >> o) || o < 0 || o > 7) return fail("an order's octant is 0 to 7");
            unsigned long fi = 0;
            auto& order = model->order[static_cast<size_t>(o)];
            order.clear();
            while (words >> fi) order.push_back(static_cast<uint16_t>(fi));
        } else {
            return fail("unknown line \"" + what + "\"");
        }
    }
    for (const auto& [id, m] : pack.models) {
        if (const std::string why = check_model(m); !why.empty()) {
            error = model_name(id) + ": " + why;
            return std::nullopt;
        }
    }
    for (const auto& [routine, m] : pack.objects) {
        if (const std::string why = check_model(m); !why.empty()) {
            error = "object " + hex(routine, 4) + ": " + why;
            return std::nullopt;
        }
    }
    return pack;
}

std::vector<std::pair<uint16_t, int>> object_models(const ModelPack& pack) {
    std::vector<std::pair<uint16_t, int>> out;
    for (const auto& [routine, model] : pack.objects) out.emplace_back(routine, kModelCount + static_cast<int>(out.size()));
    return out;
}

void install_object_draws(Cpu& cpu, const ModelPack& pack) {
    for (const auto& [routine, id] : object_models(pack)) {
        cpu.add_watch(Cpu::linear(kEntrySeg, routine), [id = id](Cpu& c) {
            c.regs.r[host::AX] = static_cast<uint16_t>(id);
            c.regs.ip = kDrawUnrotated;
        });
    }
}

bool write_models(Memory& m, const ModelPack& pack, std::string& error) {
    uint32_t at = 0;
    const auto put = [&](uint16_t w) {
        if (at + 2 > 0x10000) return false;
        wr16(m, kPackSeg, static_cast<uint16_t>(at), w);
        at += 2;
        return true;
    };
    // The objects' table slots and headers go where the model after the table has its data: it moves
    // first, with the edited ones (as the game has it, if it isn't one of them).
    std::map<int, ModelData> models = pack.models;
    if (!pack.objects.empty()) {
        if (pack.objects.size() > kMaxObjects) {
            error = "more than " + std::to_string(kMaxObjects) + " objects";
            return false;
        }
        int mover = -1;
        for (int id = 0; id < kModelCount && mover < 0; ++id) {
            const uint16_t h = near_header(m, id);
            if (h && h != 0xFFFF && rd16(m, kModelSeg, h) == kModelSeg && rd16(m, kModelSeg, static_cast<uint16_t>(h + 4)) == kTableEnd)
                mover = id;
        }
        if (mover < 0 && pack.models.empty()) {
            error = "no room for the objects in this game's model table";
            return false;
        }
        if (mover >= 0) {
            const uint16_t h = near_header(m, mover);
            if (h < kTableEnd + 16 * pack.objects.size()) {
                error = "no room for " + std::to_string(pack.objects.size()) + " objects in this game's model table";
                return false;
            }
            if (!models.count(mover)) {
                auto as_is = read_model(m, mover);
                if (!as_is) {
                    error = model_name(mover) + " couldn't be moved to make room for the objects";
                    return false;
                }
                models[mover] = std::move(*as_is);
            }
        }
    }
    const auto write_one = [&](ModelData model, uint16_t header, const std::string& name) {
        if (const std::string why = check_model(model); !why.empty()) {
            error = name + ": " + why;
            return false;
        }
        bool no_order = true;
        for (const auto& o : model.order) no_order = no_order && o.empty();
        if (no_order) make_orders(model);
        bool ok = true;
        const auto vptr = static_cast<uint16_t>(at);
        for (const auto& v : model.verts) {
            for (const int16_t c : v) ok = ok && put(static_cast<uint16_t>(c));
        }
        std::vector<uint16_t> face_at;
        for (const auto& f : model.faces) {
            face_at.push_back(static_cast<uint16_t>(at));
            ok = ok && put(f.flags) && put(static_cast<uint16_t>(f.colour | f.outline << 8));
            for (const auto& prim : f.prims) {
                const auto n = static_cast<uint16_t>(f.lines() ? prim.size() - 1 : prim.size());
                ok = ok && put(n);
                for (const uint16_t v : prim) ok = ok && put(static_cast<uint16_t>(v * 4));
                if (!f.lines()) ok = ok && put(static_cast<uint16_t>(prim.front() * 4));
            }
            ok = ok && put(0xFFFF);
        }
        const auto optr = static_cast<uint16_t>(at);
        for (int o = 0; o < 8; ++o) ok = ok && put(0);  // (filled in below)
        for (int o = 0; o < 8; ++o) {
            if (!ok) break;
            wr16(m, kPackSeg, static_cast<uint16_t>(optr + 2 * o), static_cast<uint16_t>(at));
            ok = put(0xFFFF);  // no vertices skipped
            for (const uint16_t f : model.order[static_cast<size_t>(o)]) ok = ok && put(face_at[f]);
            ok = ok && put(0xFFFF);
        }
        if (!ok) {
            error = "the edited models are more than the 64 KB the game has room for";
            return false;
        }
        wr16(m, kModelSeg, header, kPackSeg);
        wr16(m, kModelSeg, static_cast<uint16_t>(header + 2), static_cast<uint16_t>(model.verts.size()));
        wr16(m, kModelSeg, static_cast<uint16_t>(header + 4), vptr);
        wr16(m, kModelSeg, static_cast<uint16_t>(header + 6), optr);
        return true;
    };
    for (const auto& [id, model] : models) {
        const uint16_t header = near_header(m, id);
        if (header == 0 || header == 0xFFFF || rd16(m, kModelSeg, header) == 0) {
            error = model_name(id) + ": not in this game's model table";
            return false;
        }
        if (!write_one(model, header, model_name(id))) return false;
    }
    // The objects: slot kModelCount + k at the table's end (near, mid and far all the one mesh), its
    // header after the slots.
    const auto count = static_cast<uint16_t>(pack.objects.size());
    uint16_t k = 0;
    for (const auto& [routine, model] : pack.objects) {
        const auto slot = static_cast<uint16_t>(kTableEnd + 8 * k), header = static_cast<uint16_t>(kTableEnd + 8 * count + 8 * k);
        for (int i = 0; i < 3; ++i) wr16(m, kModelSeg, static_cast<uint16_t>(slot + 2 * i), header);
        wr16(m, kModelSeg, static_cast<uint16_t>(slot + 6), kObjectFarColour);
        if (!write_one(model, header, "object " + hex(routine, 4))) return false;
        ++k;
    }
    return true;
}

bool run_until_started(host::Machine& machine, uint64_t max_ns) {
    bool started = false;
    const auto watch = machine.cpu().add_watch(Cpu::linear(kEntrySeg, kEntry), [&started](Cpu&) { started = true; });
    for (uint64_t ran = 0; !started && ran < max_ns; ran += 10'000'000) machine.run_for(10'000'000);
    machine.cpu().remove_watch(watch);
    return started;
}

void install_model_pack(host::Machine& machine, ModelPack pack) {
    install_object_draws(machine.cpu(), pack);
    machine.cpu().add_watch(Cpu::linear(kEntrySeg, kEntry), [pack = std::move(pack)](Cpu& c) {
        std::string error;
        if (!write_models(c.memory(), pack, error)) std::fprintf(stderr, "Objects: %s\n", error.c_str());
    });
}

}  // namespace vette::game
