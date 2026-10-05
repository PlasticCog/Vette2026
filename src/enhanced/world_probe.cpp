#include "enhanced/world_probe.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include "host/cpu.h"

namespace vette::enhanced {

using host::Cpu;

namespace {

// Code offsets (segment 3009) of the renderer entry points (notes 03, notes 05 section 5).
constexpr uint16_t kXformPacked = 0x3879;
constexpr uint16_t kXformPlain = 0x393F;
constexpr uint16_t kXformScreen = 0x3987;  // plain list in camera axes; unused by the map's objects
constexpr uint16_t kFill = 0xB5B3;
constexpr uint16_t kFillBx = 0xB5BC;
constexpr uint16_t kFillAlt = 0xB636;
constexpr uint16_t kFillAltBx = 0xB63F;
constexpr uint16_t kVis5 = 0x3B78;
constexpr uint16_t kVis6 = 0x3BF5;
constexpr uint16_t kLines = 0x405E;
constexpr uint16_t kModelYaw = 0xB9BA;
constexpr uint16_t kModelRot = 0xB9D6;
constexpr uint16_t kModelFlat = 0xB9E6;
constexpr uint16_t kDrawModel = 0xB765;
constexpr uint16_t kWorldToCamera = 0x3917;
constexpr uint16_t kObjectMatrix = 0x3DF9;
constexpr uint16_t kCompoundCallA = 0x2A9C;  // `call bx` in the Golden Gate list walker
constexpr uint16_t kCompoundCallB = 0x2C7A;  // `call bx` in the Bay Bridge list walker
constexpr uint16_t kSentinel = 0xFFF0;       // return address of a run
// Deeper renderer stages: reaching one means an object drew through a path that isn't hooked.
constexpr uint16_t kBadPaths[] = {0xA685, 0xB4B2, 0x5170, 0x3A64, 0x3D2F, 0x9F3B, 0xB6E1};

constexpr uint16_t kStackSeg = 0x9000;
constexpr int64_t kSlice = 20000;
constexpr int kMaxSlices = 100;  // 2M cycles per run

uint32_t code_linear(uint16_t off) { return Cpu::linear(addr::kCodeSeg, off); }

} // namespace

class Tracer::NullIo final : public host::IoBus {
public:
    uint8_t in8(uint16_t) override { return 0xFF; }
    void out8(uint16_t, uint8_t) override {}
};

uint64_t Trace::signature() const {
    Hash64 h;
    for (const TraceEvent& e : events) {
        h.byte(static_cast<uint8_t>(e.op));
        h.byte(e.colour);
        h.word(e.addr);
        h.word(e.count);
        h.word(e.aux);
        for (const int16_t p : e.pos) {
            h.word(static_cast<uint16_t>(p));
        }
        h.word(e.angle_addr);
        if (e.op == TraceEvent::Op::Model && e.aux == 0) {
            h.word(static_cast<uint16_t>(e.angles[0]));  // the stub's constant yaw
        }
        for (uint32_t i = 0; i < e.data_count; ++i) {
            h.word(static_cast<uint16_t>(data[e.data_first + i]));
        }
    }
    return h.h;
}

Tracer::Tracer(const uint8_t* ram) : io_(std::make_unique<NullIo>()) {
    cpu_ = std::make_unique<Cpu>(mem_, *io_);
    load(ram);
    install_hooks();
}

Tracer::~Tracer() = default;

void Tracer::load(const uint8_t* ram) {
    std::copy_n(ram, host::Memory::kSize, mem_.ram());
    journal_.clear();
    journaling_ = false;
    mem_.set_journal(nullptr);
}

host::Memory& Tracer::memory() { return mem_; }

uint16_t Tracer::rd16(uint16_t seg, uint16_t off) {
    return static_cast<uint16_t>(rd8(seg, off) | rd8(seg, static_cast<uint16_t>(off + 1)) << 8);
}
uint8_t Tracer::rd8(uint16_t seg, uint16_t off) { return mem_.read8(Cpu::linear(seg, off)); }
void Tracer::wr16(uint16_t seg, uint16_t off, uint16_t v) {
    wr8(seg, off, static_cast<uint8_t>(v));
    wr8(seg, static_cast<uint16_t>(off + 1), static_cast<uint8_t>(v >> 8));
}
void Tracer::wr8(uint16_t seg, uint16_t off, uint8_t v) { mem_.write8(Cpu::linear(seg, off), v); }

void Tracer::begin() {
    journal_.clear();
    journaling_ = true;
    mem_.set_journal(&journal_);
}

void Tracer::end() {
    mem_.set_journal(nullptr);
    for (auto it = journal_.rbegin(); it != journal_.rend(); ++it) {
        mem_.ram()[it->linear] = it->old;
    }
    journal_.clear();
    journaling_ = false;
}

bool Tracer::wrote(uint16_t seg, uint16_t off) const {
    const uint32_t lin = Cpu::linear(seg, off);
    for (size_t j = journal_mark_; j < journal_.size(); ++j) {
        if (journal_[j].linear == lin) {
            return true;
        }
    }
    return false;
}

void Tracer::ret_near() { cpu_->regs.ip = cpu_->pop16(); }

void Tracer::record(TraceEvent ev, const int16_t* data, size_t n) {
    if (!cur_) {
        return;
    }
    ev.data_first = static_cast<uint32_t>(cur_->data.size());
    ev.data_count = static_cast<uint32_t>(n);
    cur_->data.insert(cur_->data.end(), data, data + n);
    cur_->events.push_back(ev);
}

void Tracer::bad(uint16_t where) {
    if (!cur_) {
        return;
    }
    TraceEvent ev;
    ev.op = TraceEvent::Op::Bad;
    ev.addr = where;
    record(ev);
    if (cur_->error.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "unhooked renderer path 3009:%04X", where);
        cur_->error = buf;
    }
}

void Tracer::install_hooks() {
    Cpu& cpu = *cpu_;
    using host::AX;
    using host::BX;
    using host::CX;
    using host::DS;
    using host::SI;

    const auto rel = [this](uint16_t v, int axis) {
        return static_cast<int16_t>(static_cast<uint16_t>(v - static_cast<uint16_t>(entry_[static_cast<size_t>(axis)])));
    };
    const auto ds_word = [this](uint16_t off) { return rd16(addr::kDataSeg, off); };
    const auto obj_pos = [this, rel, ds_word] {
        return std::array<int16_t, 3>{rel(ds_word(addr::kObjPos), 0), rel(ds_word(addr::kObjPos + 2), 1),
                                      rel(ds_word(addr::kObjPos + 4), 2)};
    };

    cpu.set_code_hook(code_linear(kSentinel), [this](Cpu& c) {
        done_ = true;
        c.request_stop();
    });

    cpu.set_code_hook(code_linear(kXformPacked), [this, rel, ds_word](Cpu& c) {
        const uint16_t ds = c.regs.s[DS];
        const uint16_t bx = c.regs.r[BX];
        TraceEvent ev;
        ev.op = TraceEvent::Op::Xform;
        ev.addr = bx;
        ev.count = ds_word(addr::kVertexCount);
        for (int i = 0; i < 3; ++i) {
            ev.pos[static_cast<size_t>(i)] = rel(rd16(ds, static_cast<uint16_t>(bx + 2 * i)), i);
        }
        std::vector<int16_t> words(static_cast<size_t>(ev.count) * 3);
        for (size_t i = 0; i < words.size(); ++i) {
            words[i] = static_cast<int16_t>(rd16(ds, static_cast<uint16_t>(bx + 6 + 2 * i)));
        }
        record(ev, words.data(), words.size());
        ret_near();
    });

    cpu.set_code_hook(code_linear(kXformPlain), [this, rel, ds_word](Cpu& c) {
        const uint16_t ds = c.regs.s[DS];
        const uint16_t bx = c.regs.r[BX];
        TraceEvent ev;
        ev.op = TraceEvent::Op::Plain;
        ev.addr = bx;
        ev.count = ds_word(addr::kVertexCount);
        ev.aux = c.regs.r[CX];
        for (int i = 0; i < 3; ++i) {
            ev.pos[static_cast<size_t>(i)] = rel(static_cast<uint16_t>(last_point_[static_cast<size_t>(i)]), i);
        }
        ev.angle_addr = angle_addr_;
        ev.angles = angles_;
        ev.angle_prior = angle_prior_;
        for (int i = 0; i < 9; ++i) {
            ev.matrix[static_cast<size_t>(i)] = static_cast<int16_t>(ds_word(static_cast<uint16_t>(addr::kPlainMatrix + 2 * i)));
        }
        for (int i = 0; i < 3; ++i) {
            ev.translation[static_cast<size_t>(i)] =
                static_cast<int16_t>(ds_word(static_cast<uint16_t>(addr::kCameraPoint + 2 * i)));
        }
        std::vector<int16_t> words(static_cast<size_t>(ev.count) * 3);
        for (size_t i = 0; i < words.size(); ++i) {
            words[i] = static_cast<int16_t>(rd16(ds, static_cast<uint16_t>(bx + 2 * i)));
        }
        record(ev, words.data(), words.size());
        ret_near();
    });

    const auto fill = [this](Cpu& c, TraceEvent::Op op, bool load_bx) {
        const uint16_t ds = c.regs.s[DS];
        const uint16_t si = c.regs.r[SI];
        TraceEvent ev;
        ev.op = op;
        ev.addr = si;
        ev.colour = static_cast<uint8_t>(c.regs.r[AX]);
        ev.aux = load_bx ? uint16_t{0x266E} : c.regs.r[BX];
        std::vector<int16_t> words;
        uint16_t p = si;
        for (int guard = 0; guard < 64; ++guard) {
            const uint16_t n = rd16(ds, p);
            if (n == 0xFFFF || n > 64) {
                break;
            }
            words.push_back(static_cast<int16_t>(n));
            for (int i = 0; i <= n; ++i) {
                words.push_back(static_cast<int16_t>(rd16(ds, static_cast<uint16_t>(p + 2 + 2 * i))));
            }
            p = static_cast<uint16_t>(p + 2 + 2 * (n + 1));
        }
        record(ev, words.data(), words.size());
        ret_near();
    };
    cpu.set_code_hook(code_linear(kFill), [fill](Cpu& c) { fill(c, TraceEvent::Op::Fill, true); });
    cpu.set_code_hook(code_linear(kFillBx), [fill](Cpu& c) { fill(c, TraceEvent::Op::Fill, false); });
    cpu.set_code_hook(code_linear(kFillAlt), [fill](Cpu& c) { fill(c, TraceEvent::Op::FillAlt, true); });
    cpu.set_code_hook(code_linear(kFillAltBx), [fill](Cpu& c) { fill(c, TraceEvent::Op::FillAlt, false); });

    const auto vis = [this, ds_word](TraceEvent::Op op, int size) {
        TraceEvent ev;
        ev.op = op;
        ev.addr = ds_word(addr::kFaceTable);
        ev.count = rd8(addr::kDataSeg, addr::kFaceCount);
        std::vector<int16_t> bytes(static_cast<size_t>(ev.count) * static_cast<size_t>(size));
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = rd8(addr::kDataSeg, static_cast<uint16_t>(ev.addr + i));
        }
        for (int i = 0; i < ev.count; ++i) {
            const bool hidden = vis_calls_ == hide_call_ && i == hide_face_;
            wr8(addr::kDataSeg, static_cast<uint16_t>(addr::kFaceFlags + i), hidden ? 0 : 1);
        }
        ++vis_calls_;
        record(ev, bytes.data(), bytes.size());
        ret_near();
    };
    cpu.set_code_hook(code_linear(kVis5), [vis](Cpu&) { vis(TraceEvent::Op::Vis5, 5); });
    cpu.set_code_hook(code_linear(kVis6), [vis](Cpu&) { vis(TraceEvent::Op::Vis6, 6); });

    cpu.set_code_hook(code_linear(kLines), [this, ds_word](Cpu&) {
        TraceEvent ev;
        ev.op = TraceEvent::Op::Lines;
        ev.addr = ds_word(addr::kLineList);
        ev.colour = rd8(addr::kDataSeg, addr::kLineColour);
        ev.count = rd8(addr::kDataSeg, static_cast<uint16_t>(ev.addr + 1));
        std::vector<int16_t> words(ev.count);
        for (size_t i = 0; i < words.size(); ++i) {
            words[i] = static_cast<int16_t>(ds_word(static_cast<uint16_t>(ev.addr + 2 + 2 * i)));
        }
        record(ev, words.data(), words.size());
        ret_near();
    });

    // Models: recorded on entry. In extraction they are skipped right there; with exact_models the
    // entry runs (object matrix, LOD choice, far colour patch) and draw_model is skipped instead,
    // after its inputs are copied into the event.
    const auto model = [this, obj_pos, ds_word](Cpu& c, uint16_t kind) {
        TraceEvent ev;
        ev.op = TraceEvent::Op::Model;
        ev.addr = c.regs.r[AX];
        ev.aux = kind;
        ev.pos = obj_pos();
        if (kind != 2) {
            ev.angles[0] = static_cast<int16_t>(ds_word(addr::kObjAngles));
        }
        if (kind == 1) {
            ev.angles[1] = static_cast<int16_t>(ds_word(addr::kObjAngles + 2));
            ev.angles[2] = static_cast<int16_t>(ds_word(addr::kObjAngles + 4));
        }
        ev.outline_enable = rd8(addr::kDataSeg, 0xE0D8);  // the opponent clears it around its own draw
        record(ev);
    };
    struct ModelEntry {
        uint16_t at, kind;
    };
    const ModelEntry model_entries[] = {{kModelYaw, 0}, {kModelRot, 1}, {kModelFlat, 2}};
    for (const ModelEntry& me : model_entries) {
        cpu.add_watch(code_linear(me.at), [this, model, kind = me.kind](Cpu& c) {
            model(c, kind);
            if (!exact_models_) {
                ret_near();
            }
        });
    }
    cpu.set_code_hook(code_linear(kDrawModel), [this, ds_word](Cpu& c) {
        if (!exact_models_) {
            bad(kDrawModel);
            return;
        }
        if (cur_ && !cur_->events.empty() && cur_->events.back().op == TraceEvent::Op::Model) {
            TraceEvent& ev = cur_->events.back();
            ev.model_header = c.regs.r[SI];
            ev.outline_enable = rd8(addr::kDataSeg, 0xE0D8);
            for (int i = 0; i < 9; ++i) {
                ev.matrix[static_cast<size_t>(i)] =
                    static_cast<int16_t>(ds_word(static_cast<uint16_t>(addr::kModelMatrix + 2 * i)));
            }
            for (int i = 0; i < 3; ++i) {
                ev.translation[static_cast<size_t>(i)] =
                    static_cast<int16_t>(ds_word(static_cast<uint16_t>(addr::kModelMatrix + 18 + 2 * i)));
            }
        }
        ret_near();
    });

    for (const uint16_t at : {kCompoundCallA, kCompoundCallB}) {
        cpu.set_code_hook(code_linear(at), [this, obj_pos](Cpu& c) {
            TraceEvent ev;
            ev.op = TraceEvent::Op::SubCall;
            ev.addr = c.regs.r[BX];
            ev.pos = obj_pos();
            record(ev);
            c.regs.ip = static_cast<uint16_t>(c.regs.ip + 2);  // skip the call
        });
    }

    // Watched helpers.
    cpu.add_watch(code_linear(kWorldToCamera), [this](Cpu& c) {
        const uint16_t ds = c.regs.s[DS], si = c.regs.r[SI];
        for (int i = 0; i < 3; ++i) {
            last_point_[static_cast<size_t>(i)] = static_cast<int16_t>(rd16(ds, static_cast<uint16_t>(si + 2 * i)));
        }
    });
    cpu.add_watch(code_linear(kObjectMatrix), [this](Cpu& c) {
        const uint16_t ds = c.regs.s[DS], si = c.regs.r[SI];
        angle_addr_ = si;
        for (int i = 0; i < 3; ++i) {
            const auto off = static_cast<uint16_t>(si + 2 * i);
            angles_[static_cast<size_t>(i)] = static_cast<int16_t>(rd16(ds, off));
            angle_prior_[static_cast<size_t>(i)] = angles_[static_cast<size_t>(i)];
            // The value before this run touched it: the first journaled write of each byte.
            const uint32_t lo = Cpu::linear(ds, off), hi = Cpu::linear(ds, static_cast<uint16_t>(off + 1));
            int got = 0;
            uint16_t prior = static_cast<uint16_t>(angles_[static_cast<size_t>(i)]);
            for (size_t j = journal_mark_; j < journal_.size() && got != 3; ++j) {
                if (journal_[j].linear == lo && !(got & 1)) {
                    prior = static_cast<uint16_t>((prior & 0xFF00) | journal_[j].old);
                    got |= 1;
                } else if (journal_[j].linear == hi && !(got & 2)) {
                    prior = static_cast<uint16_t>((prior & 0x00FF) | (journal_[j].old << 8));
                    got |= 2;
                }
            }
            angle_prior_[static_cast<size_t>(i)] = static_cast<int16_t>(prior);
        }
    });
    cpu.add_watch(code_linear(kXformScreen), [this](Cpu&) { bad(kXformScreen); });
    for (const uint16_t at : kBadPaths) {
        cpu.add_watch(code_linear(at), [this, at](Cpu&) { bad(at); });
    }
}

bool Tracer::call(uint16_t routine, host::Registers& regs) {
    Trace scratch;
    cur_ = &scratch;
    done_ = false;
    host::Registers& r = cpu_->regs;
    r = regs;
    r.s[host::CS] = addr::kCodeSeg;
    r.s[host::DS] = addr::kDataSeg;
    r.s[host::ES] = addr::kDataSeg;
    r.s[host::SS] = kStackSeg;
    r.r[host::SP] = 0xFFFE;
    r.ip = routine;
    r.flags = 0x0002;
    cpu_->push16(kSentinel);
    for (int i = 0; i < kMaxSlices && !done_; ++i) {
        cpu_->run(kSlice);
    }
    cur_ = nullptr;
    regs = r;
    return done_;
}

bool Tracer::run(uint16_t routine, std::array<int16_t, 3> entry, Trace& out) {
    out.clear();
    cur_ = &out;
    entry_ = entry;
    done_ = false;
    vis_calls_ = 0;
    angle_addr_ = 0;
    angles_ = {};
    angle_prior_ = {};
    last_point_ = {};
    journal_mark_ = journal_.size();
    ++runs_;

    host::Registers& r = cpu_->regs;
    r.s[host::CS] = addr::kCodeSeg;
    r.s[host::DS] = addr::kDataSeg;
    r.s[host::ES] = addr::kDataSeg;
    r.s[host::SS] = kStackSeg;
    r.r[host::SP] = 0xFFFE;
    r.ip = routine;
    r.flags = 0x0002;
    cpu_->push16(kSentinel);
    for (int i = 0; i < kMaxSlices && !done_; ++i) {
        cpu_->run(kSlice);
    }
    cur_ = nullptr;
    if (!done_) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "did not return (CS:IP %04X:%04X)", r.s[host::CS], r.ip);
        out.error = buf;
        return false;
    }
    return out.error.empty();
}

} // namespace vette::enhanced
