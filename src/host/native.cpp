#include "host/native.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <unordered_map>

namespace vette::host {
namespace {

constexpr uint64_t kReturnBudgetCycles = 5'000'000;  // ~0.4 s at 12 MHz: far beyond any leaf routine
constexpr uint32_t kDeadStackBytes = 0x400;          // stack below the returned SP is scratch
constexpr int kDetailedReports = 3;                  // per function; later mismatches are only counted
constexpr uint16_t kDataSegment = 0x124A + Machine::kLoadSegment;

uint16_t read_word(Memory& mem, uint16_t seg, uint16_t off) {
    return static_cast<uint16_t>(mem.read8(Cpu::linear(seg, off)) |
                                 mem.read8(Cpu::linear(seg, static_cast<uint16_t>(off + 1))) << 8);
}

void emulate_return(Cpu& cpu, const NativeFunction& fn) {
    Registers& r = cpu.regs;
    r.ip = cpu.pop16();
    if (fn.far) {
        r.s[CS] = cpu.pop16();
    }
    r.r[SP] = static_cast<uint16_t>(r.r[SP] + fn.ret_pop);
}

std::string describe_address(uint32_t linear) {
    char buf[32];
    const uint32_t ds_base = kDataSegment * 16u;
    if (linear >= ds_base && linear - ds_base < 0x10000) {
        std::snprintf(buf, sizeof buf, "DS:%04X", linear - ds_base);  // the game's data segment (124A)
    } else {
        std::snprintf(buf, sizeof buf, "%05X", linear);
    }
    return buf;
}

std::string describe_regs(const Registers& r) {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X FL=%04X",
                  r.r[AX], r.r[BX], r.r[CX], r.r[DX], r.r[SI], r.r[DI], r.r[BP], r.r[SP], r.s[DS], r.s[ES],
                  r.flags);
    return buf;
}

} // namespace

struct NativeRunner::Entry {
    NativeFunction fn;
    Mode mode = Mode::Original;
    uint32_t linear = 0;
    Stats stats;
};

struct NativeRunner::Active {
    Entry* entry = nullptr;
    Registers regs_in;
    uint64_t start_cycles = 0;
    uint16_t ret_cs = 0, ret_ip = 0, ret_ss = 0, ret_sp = 0;  // CS:IP and SS:SP right after the return
    uint32_t ret_linear = 0;
    std::vector<Memory::JournalEntry> journal;
};

NativeRunner::NativeRunner(Machine& machine) : machine_(machine) {}

NativeRunner::~NativeRunner() {
    Cpu& cpu = machine_.cpu();
    for (const auto& e : entries_) {
        cpu.clear_code_hook(e->linear);
        cpu.clear_watch(e->linear);
    }
    if (active_) {
        cpu.clear_watch(active_->ret_linear);
        cpu.set_irq_inhibit(false);
        machine_.memory().set_journal(nullptr);
    }
}

void NativeRunner::install(const NativeFunction& fn, Mode mode) {
    auto entry = std::make_unique<Entry>();
    entry->fn = fn;
    entry->mode = mode;
    entry->linear = Cpu::linear(static_cast<uint16_t>(fn.seg + Machine::kLoadSegment), fn.off);
    Entry& e = *entry;
    entries_.push_back(std::move(entry));

    Cpu& cpu = machine_.cpu();
    if (mode == Mode::Native) {
        cpu.set_code_hook(e.linear, [&e](Cpu& c) {
            ++e.stats.calls;
            e.fn.impl(c);
            emulate_return(c, e.fn);
        });
    } else if (mode == Mode::Verify) {
        cpu.set_watch(e.linear, [this, &e](Cpu&) { on_entry(e); });
    }
}

void NativeRunner::on_entry(Entry& e) {
    if (active_) {
        return;  // nested inside another verified call: run the original only
    }
    Cpu& cpu = machine_.cpu();
    Memory& mem = machine_.memory();
    const Registers& r = cpu.regs;

    auto a = std::make_unique<Active>();
    a->entry = &e;
    a->regs_in = r;
    a->start_cycles = cpu.total_cycles();
    a->ret_ip = read_word(mem, r.s[SS], r.r[SP]);
    a->ret_cs = e.fn.far ? read_word(mem, r.s[SS], static_cast<uint16_t>(r.r[SP] + 2)) : r.s[CS];
    a->ret_ss = r.s[SS];
    a->ret_sp = static_cast<uint16_t>(r.r[SP] + (e.fn.far ? 4 : 2) + e.fn.ret_pop);
    a->ret_linear = Cpu::linear(a->ret_cs, a->ret_ip);
    for (const auto& other : entries_) {
        if (other->linear == a->ret_linear) {
            return;  // the return address is another function's entry; its watch can't be shared
        }
    }

    if (e.fn.touches_vram) {
        if (!ega_before_) {
            ega_before_ = std::make_unique<Ega>();
            ega_original_ = std::make_unique<Ega>();
        }
        ega_before_->copy_state_from(machine_.ega());
    }
    mem.set_journal(&a->journal);
    cpu.set_irq_inhibit(true);
    active_ = std::move(a);
    cpu.set_watch(active_->ret_linear, [this, &e](Cpu& c) {
        const Active* act = active_.get();
        if (act && c.regs.s[CS] == act->ret_cs && c.regs.ip == act->ret_ip && c.regs.s[SS] == act->ret_ss &&
            c.regs.r[SP] == act->ret_sp) {
            on_return(e);
        }
    });
}

void NativeRunner::on_return(Entry& e) {
    Active& a = *active_;
    Cpu& cpu = machine_.cpu();
    Memory& mem = machine_.memory();
    uint8_t* ram = mem.ram();
    ++e.stats.calls;

    // 1. The original's results: registers, and the final value of every byte it wrote.
    const Registers regs_original = cpu.regs;
    std::unordered_map<uint32_t, uint8_t> before;  // address -> value before the call
    for (const auto& j : a.journal) {
        before.try_emplace(j.linear, j.old);
    }
    std::unordered_map<uint32_t, uint8_t> original;
    for (const auto& [addr, old] : before) {
        original[addr] = ram[addr];
    }
    if (e.fn.touches_vram) {
        ega_original_->copy_state_from(machine_.ega());
    }

    // 2. Roll back to the state at entry.
    for (auto it = a.journal.rbegin(); it != a.journal.rend(); ++it) {
        ram[it->linear] = it->old;
    }
    if (e.fn.touches_vram) {
        machine_.ega().copy_state_from(*ega_before_);
    }
    cpu.regs = a.regs_in;

    // 3. The native version from the same state.
    std::vector<Memory::JournalEntry> native_journal;
    mem.set_journal(&native_journal);
    e.fn.impl(cpu);
    emulate_return(cpu, e.fn);
    mem.set_journal(nullptr);
    for (const auto& j : native_journal) {
        if (!original.count(j.linear)) {
            original.emplace(j.linear, before.count(j.linear) ? before[j.linear] : j.old);
        }
    }

    // 4. Compare.
    std::string diff;
    const Registers& rn = cpu.regs;
    const Registers& ro = regs_original;
    static constexpr const char* kRegNames[] = {"AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"};
    static constexpr const char* kSegNames[] = {"ES", "CS", "SS", "DS"};
    char line[96];
    for (int i = 0; i < 8; ++i) {
        if (rn.r[i] != ro.r[i]) {
            std::snprintf(line, sizeof line, "  %s: native %04X, original %04X\n", kRegNames[i], rn.r[i], ro.r[i]);
            diff += line;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (rn.s[i] != ro.s[i]) {
            std::snprintf(line, sizeof line, "  %s: native %04X, original %04X\n", kSegNames[i], rn.s[i], ro.s[i]);
            diff += line;
        }
    }
    if (rn.ip != ro.ip) {
        std::snprintf(line, sizeof line, "  IP: native %04X, original %04X\n", rn.ip, ro.ip);
        diff += line;
    }
    if ((rn.flags ^ ro.flags) & e.fn.flags_mask) {
        std::snprintf(line, sizeof line, "  FLAGS: native %04X, original %04X (mask %04X)\n", rn.flags, ro.flags,
                      e.fn.flags_mask);
        diff += line;
    }
    // Dead stack: the kDeadStackBytes below the returned SP, as 16-bit offsets in SS (VETTE's stack
    // sits at the bottom of its segment, so a linear range would wrap wrongly).
    const uint32_t ss_base = ro.s[SS] * 16u;
    auto is_dead_stack = [&](uint32_t addr) {
        const uint32_t off = (addr - ss_base) & Memory::kMask;
        if (off > 0xFFFF) {
            return false;  // outside the stack segment
        }
        const auto below = static_cast<uint16_t>(ro.r[SP] - off);
        return below >= 1 && below <= kDeadStackBytes;
    };
    std::vector<uint32_t> addrs;
    for (const auto& [addr, value] : original) {
        if (is_dead_stack(addr)) {
            ram[addr] = value;  // dead stack: not compared, but keep the original's bytes so RAM stays identical
        } else if (ram[addr] != value) {
            addrs.push_back(addr);
        }
    }
    std::sort(addrs.begin(), addrs.end());
    for (size_t i = 0; i < addrs.size() && i < 8; ++i) {
        std::snprintf(line, sizeof line, "  %s: native %02X, original %02X\n", describe_address(addrs[i]).c_str(),
                      ram[addrs[i]], original[addrs[i]]);
        diff += line;
    }
    if (addrs.size() > 8) {
        diff += "  ... " + std::to_string(addrs.size() - 8) + " more bytes differ\n";
    }
    if (e.fn.touches_vram) {
        const std::string ega_diff = machine_.ega().diff_state(*ega_original_);
        if (!ega_diff.empty()) {
            diff += "  EGA:\n" + ega_diff;
        }
    }

    // 5. On a mismatch, report it and keep the original's result.
    if (!diff.empty()) {
        ++e.stats.mismatches;
        if (report_ && e.stats.mismatches <= kDetailedReports) {
            report_(std::string("MISMATCH ") + e.fn.name + " (call " + std::to_string(e.stats.calls) +
                    ")\n  in:  " + describe_regs(a.regs_in) + "\n" + diff);
        }
        for (const auto& [addr, value] : original) {
            ram[addr] = value;
        }
        cpu.regs = regs_original;
        if (e.fn.touches_vram) {
            machine_.ega().copy_state_from(*ega_original_);
        }
    }

    cpu.clear_watch(a.ret_linear);
    cpu.set_irq_inhibit(false);
    active_.reset();
}

void NativeRunner::poll() {
    if (!active_ || machine_.cpu().total_cycles() - active_->start_cycles < kReturnBudgetCycles) {
        return;
    }
    Entry& e = *active_->entry;
    Cpu& cpu = machine_.cpu();
    cpu.clear_watch(active_->ret_linear);
    cpu.clear_watch(e.linear);
    cpu.set_irq_inhibit(false);
    machine_.memory().set_journal(nullptr);
    active_.reset();
    e.mode = Mode::Original;
    if (report_) {
        report_(std::string("ABANDONED ") + e.fn.name +
                ": the original did not return to its caller within the cycle budget; verification disabled");
    }
}

NativeRunner::Stats NativeRunner::stats(const char* name) const {
    for (const auto& e : entries_) {
        if (std::string_view(e->fn.name) == name) {
            return e->stats;
        }
    }
    return {};
}

std::string NativeRunner::summary() const {
    std::string out;
    for (const auto& e : entries_) {
        const char* mode = e->mode == Mode::Verify ? "verify" : e->mode == Mode::Native ? "native" : "original";
        char line[128];
        std::snprintf(line, sizeof line, "%-28s %-8s %10llu calls %8llu mismatches\n", e->fn.name, mode,
                      static_cast<unsigned long long>(e->stats.calls),
                      static_cast<unsigned long long>(e->stats.mismatches));
        out += line;
    }
    return out;
}

} // namespace vette::host
