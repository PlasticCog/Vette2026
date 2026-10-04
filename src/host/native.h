#pragma once
// Phase 2: native C++ replacements for original VETTE.EXE functions, and the harness that proves
// them equivalent.
//
// A NativeFunction implements one original routine against the emulated machine state (registers
// and memory), up to but not including its return. Each one runs in one of three modes:
//   Original - not installed; the original code runs.
//   Native   - a code hook replaces the original. The native code costs no emulated time, so
//              timing-sensitive comparisons should use Verify.
//   Verify   - on every call the original runs first while every RAM write is journaled (plus an EGA
//              snapshot for drawing functions). The harness then rolls the machine back, runs the native
//              version from the same state, and compares registers, flags, every byte either version
//              wrote, and EGA state. A mismatch is reported and the original's result is kept, so the
//              game continues exactly as the original would. Emulated time is the original's.
//
// While a verified call runs, hardware interrupts are held pending (so they can't mix into the
// comparison) and nested verified functions run their original code.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "host/machine.h"

namespace vette::host {

struct NativeFunction {
    const char* name;           // symbol name in re/symbols.csv
    uint16_t seg, off;          // image-relative address (emulator segment = seg + load segment)
    bool far = false;           // returns with RETF instead of RET
    uint16_t ret_pop = 0;       // RET n
    bool touches_vram = false;  // snapshot and compare the EGA as well
    uint16_t flags_mask = 0xFFFF;  // FLAGS bits that must match after the call
    void (*impl)(Cpu&);         // the native body, stopping before the return
};

class NativeRunner {
public:
    enum class Mode { Original, Native, Verify };

    struct Stats {
        uint64_t calls = 0;
        uint64_t mismatches = 0;
    };

    explicit NativeRunner(Machine& machine);
    ~NativeRunner();
    NativeRunner(const NativeRunner&) = delete;
    NativeRunner& operator=(const NativeRunner&) = delete;

    void install(const NativeFunction& fn, Mode mode);

    // Call regularly (e.g. after each Machine::run_for). It abandons a verification whose original
    // didn't return within a cycle budget (a routine that leaves by an unexpected path); that
    // function then reverts to Original mode and the problem is reported.
    void poll();

    // Called with a description of each mismatch (the first few per function are detailed).
    void set_report(std::function<void(const std::string&)> report) { report_ = std::move(report); }

    Stats stats(const char* name) const;
    std::string summary() const;  // one line per installed function

private:
    struct Entry;
    struct Active;
    void on_entry(Entry& e);
    void on_return(Entry& e);

    Machine& machine_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::unique_ptr<Active> active_;  // the verification in progress, if any
    std::unique_ptr<Ega> ega_before_, ega_original_;
    std::function<void(const std::string&)> report_;
};

} // namespace vette::host
