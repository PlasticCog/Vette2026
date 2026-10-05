// SingleStepTests 80286 real-mode suite (https://github.com/SingleStepTests/80286), hardware-captured
// on a Harris N80C286-12. Fetch and convert the data first:
//     python tests/tools/fetch_singlestep.py
// The test is skipped (with a note) when the data is absent.
//
// Each test sets the initial registers and RAM, runs until the HLT that follows the instruction (the
// suite places a HLT at the fall-through address, the jump target, or the exception handler), and
// compares every register, FLAGS (masking the flags the suite's metadata marks undefined) and every
// listed RAM byte. Undefined flags are also masked in the FLAGS image an exception pushes.
//
// Exclusions (counted and reported, not failed):
//  * 286 #GP (INT 0Dh) tests: a word access at offset FFFFh, or an instruction longer than 10 bytes.
//    The host deliberately wraps within the segment like an 8086 (see cpu.cpp), so these differ by
//    design. A genuine `INT 0Dh` instruction is not excluded.
//  * Tests in the suite's revocation list.
//  * Tests whose RAM addresses collide once folded to 20 bits (the suite assumes 16 MB, the host
//    wraps at 1 MB): a handful of string tests.
// Cycle counts, bus traces and prefetch-queue state are not compared (the cycle model is approximate).
//
// Environment: VETTE_SST_DIR overrides the data directory; VETTE_SST_ONLY=00,F6.6 limits the run to
// those files; VETTE_SST_SHOW=n prints up to n failure details per file (default 3).

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "host/cpu.h"
#include "test.h"

using namespace vette::host;

namespace {

namespace fs = std::filesystem;

std::string env(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

struct SstIo final : IoBus {
    uint8_t in8(uint16_t) override { return 0xFF; }  // the test rig's bus floats high
    void out8(uint16_t, uint8_t) override {}
};

struct RamByte {
    uint32_t addr;
    bool initial;
    uint8_t init;
    uint8_t final;
};

struct SstTest {
    uint32_t idx = 0;
    std::vector<uint8_t> bytes;
    uint8_t flags = 0;
    uint8_t exception = 0xFF;
    uint32_t flag_address = 0;
    uint16_t init[14]{};
    uint16_t final[14]{};
    std::vector<RamByte> ram;
};

constexpr const char* kRegNames[14] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                       "es", "cs", "ss", "ds", "ip", "flags"};

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data) : p_(data.data()), end_(data.data() + data.size()) {}
    bool ok() const { return ok_; }
    template <typename T>
    T get() {
        T v{};
        if (end_ - p_ < static_cast<std::ptrdiff_t>(sizeof(T))) {
            ok_ = false;
            return v;
        }
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

bool load_file(const fs::path& path, std::vector<SstTest>& tests, uint16_t& flags_mask) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Reader r(data);
    std::array<uint8_t, 4> magic{};
    for (size_t k = 0; k < magic.size(); ++k) magic[k] = r.get<uint8_t>();
    if (magic != std::array<uint8_t, 4>{'V', 'S', 'S', '1'}) return false;
    const uint32_t count = r.get<uint32_t>();
    flags_mask = r.get<uint16_t>();
    r.get<uint8_t>();  // status
    r.get<uint8_t>();
    tests.resize(count);
    for (SstTest& t : tests) {
        t.idx = r.get<uint32_t>();
        t.bytes.resize(r.get<uint8_t>());
        for (uint8_t& b : t.bytes) b = r.get<uint8_t>();
        t.flags = r.get<uint8_t>();
        t.exception = r.get<uint8_t>();
        t.flag_address = r.get<uint32_t>();
        for (uint16_t& v : t.init) v = r.get<uint16_t>();
        for (uint16_t& v : t.final) v = r.get<uint16_t>();
        t.ram.resize(r.get<uint16_t>());
        for (RamByte& b : t.ram) {
            const uint32_t a = r.get<uint32_t>();
            b.addr = a & Memory::kMask;
            b.initial = (a & 0x80000000u) != 0;
            b.init = r.get<uint8_t>();
            b.final = r.get<uint8_t>();
        }
    }
    return r.ok();
}

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    std::string s;
    char buf[4];
    for (uint8_t b : bytes) {
        std::snprintf(buf, sizeof buf, "%02X ", b);
        s += buf;
    }
    return s;
}

// The opcode after any prefixes, and the byte after it.
std::pair<int, int> opcode_of(const std::vector<uint8_t>& b) {
    size_t i = 0;
    while (i < b.size() && (b[i] == 0x26 || b[i] == 0x2E || b[i] == 0x36 || b[i] == 0x3E || b[i] == 0xF0 ||
                            b[i] == 0xF1 || b[i] == 0xF2 || b[i] == 0xF3)) {
        ++i;
    }
    return {i < b.size() ? b[i] : -1, i + 1 < b.size() ? b[i + 1] : -1};
}

enum class Outcome { kPass, kFail, kExcludedGp, kExcludedRevoked, kExcludedAlias };

struct Runner {
    Memory mem;
    SstIo io;
    Cpu cpu{mem, io};
    uint8_t* ram = mem.ram();

    Outcome run(const SstTest& t, uint16_t flags_mask, std::string* why) {
        if (t.flags & 2) return Outcome::kExcludedRevoked;
        if (t.flags & 1) return Outcome::kExcludedAlias;
        if (t.exception == 13) {
            const auto [op, next] = opcode_of(t.bytes);
            if (!(op == 0xCD && next == 0x0D)) return Outcome::kExcludedGp;
        }

        cpu.reset();
        Registers& R = cpu.regs;
        for (int i = 0; i < 8; ++i) R.r[i] = t.init[i];
        for (int i = 0; i < 4; ++i) R.s[i] = t.init[8 + i];
        R.ip = t.init[12];
        R.flags = t.init[13];
        for (const RamByte& b : t.ram) {
            if (b.initial) ram[b.addr] = b.init;
        }

        cpu.run(100000);

        bool ok = true;
        std::ostringstream msg;
        if (!cpu.halted()) {
            ok = false;
            msg << " [did not reach the terminating HLT]";
        }
        uint16_t got[14];
        for (int i = 0; i < 8; ++i) got[i] = R.r[i];
        for (int i = 0; i < 4; ++i) got[8 + i] = R.s[i];
        got[12] = R.ip;
        got[13] = R.flags;
        for (int i = 0; i < 14; ++i) {
            const uint16_t mask = i == 13 ? flags_mask : 0xFFFF;
            if ((got[i] & mask) != (t.final[i] & mask)) {
                ok = false;
                char buf[96];
                std::snprintf(buf, sizeof buf, " %s=%04X(want %04X)", kRegNames[i], got[i], t.final[i]);
                msg << buf;
            }
        }
        for (const RamByte& b : t.ram) {
            uint8_t mask = 0xFF;
            if (t.exception != 0xFF) {
                if (b.addr == t.flag_address) mask = static_cast<uint8_t>(flags_mask);
                else if (b.addr == ((t.flag_address + 1) & Memory::kMask)) mask = static_cast<uint8_t>(flags_mask >> 8);
            }
            if ((ram[b.addr] & mask) != (b.final & mask)) {
                ok = false;
                char buf[64];
                std::snprintf(buf, sizeof buf, " [%05X]=%02X(want %02X)", b.addr, ram[b.addr], b.final);
                msg << buf;
            }
        }
        if (!ok) {
            // A wrong jump may have run arbitrary code: start the next test from clean RAM.
            std::memset(ram, 0, Memory::kSize);
            if (why) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "#%u %s| init ax=%04X cx=%04X dx=%04X bx=%04X sp=%04X bp=%04X si=%04X di=%04X es=%04X cs=%04X ss=%04X ds=%04X ip=%04X fl=%04X\n       ",
                              t.idx, hex_bytes(t.bytes).c_str(), t.init[0], t.init[1], t.init[2], t.init[3], t.init[4],
                              t.init[5], t.init[6], t.init[7], t.init[8], t.init[9], t.init[10], t.init[11],
                              t.init[12], t.init[13]);
                *why = buf + msg.str();
            }
        } else {
            for (const RamByte& b : t.ram) ram[b.addr] = 0;
        }
        return ok ? Outcome::kPass : Outcome::kFail;
    }

    // Bytes left non-zero outside the tests' listed addresses (stray writes). Clears them.
    size_t stray_bytes() {
        size_t n = 0;
        for (uint32_t a = 0; a < Memory::kSize; ++a) n += ram[a] != 0;
        if (n) std::memset(ram, 0, Memory::kSize);
        return n;
    }
};

}  // namespace

TEST(cpu_singlestep_80286) {
    fs::path dir = env("VETTE_SST_DIR");
    if (dir.empty()) dir = fs::path(__FILE__).parent_path() / "data" / "singlestep" / "80286";
    std::ifstream index(dir / "index.txt");
    if (!index) {
        std::printf("  SKIPPED: no SingleStepTests data in %s\n"
                    "  (run: python tests/tools/fetch_singlestep.py)\n",
                    dir.string().c_str());
        return;
    }
    const std::string only = env("VETTE_SST_ONLY");
    const std::string show_env = env("VETTE_SST_SHOW");
    const int show = show_env.empty() ? 3 : std::atoi(show_env.c_str());

    auto runner = std::make_unique<Runner>();
    std::vector<std::string> grid;
    long long total = 0, passed = 0, failed = 0, gp = 0, revoked = 0, alias = 0;
    int files = 0;
    std::string line;
    while (std::getline(index, line)) {
        std::istringstream ls(line);
        std::string name, status;
        unsigned mask = 0xFFFF;
        ls >> name >> mask >> status;
        if (name.empty()) continue;
        if (!only.empty() && ("," + only + ",").find("," + name + ",") == std::string::npos) continue;

        std::vector<SstTest> tests;
        uint16_t flags_mask = 0xFFFF;
        if (!load_file(dir / (name + ".bin"), tests, flags_mask)) {
            std::printf("  %s: cannot read %s.bin\n", name.c_str(), name.c_str());
            CHECK(false);
            continue;
        }
        ++files;
        long long f_pass = 0, f_fail = 0, f_excl = 0;
        int shown = 0;
        for (const SstTest& t : tests) {
            std::string why;
            switch (runner->run(t, flags_mask, shown < show ? &why : nullptr)) {
            case Outcome::kPass: ++f_pass; break;
            case Outcome::kFail:
                ++f_fail;
                if (shown++ < show) std::printf("  %s %s\n", name.c_str(), why.c_str());
                break;
            case Outcome::kExcludedGp: ++gp; ++f_excl; break;
            case Outcome::kExcludedRevoked: ++revoked; ++f_excl; break;
            case Outcome::kExcludedAlias: ++alias; ++f_excl; break;
            }
        }
        const size_t stray = runner->stray_bytes();
        if (stray) {
            std::printf("  %s: %zu stray byte(s) written outside the tests' RAM lists\n", name.c_str(), stray);
            CHECK(false);
        }
        total += static_cast<long long>(tests.size());
        passed += f_pass;
        failed += f_fail;
        const long long run = f_pass + f_fail;
        char cell[32];
        if (f_fail == 0) {
            std::snprintf(cell, sizeof cell, "%-5s 100%% ", name.c_str());
        } else {
            std::snprintf(cell, sizeof cell, "%-5s %5.1f%%", name.c_str(), run ? 100.0 * static_cast<double>(f_pass) / static_cast<double>(run) : 0.0);
        }
        grid.emplace_back(cell);
    }

    std::printf("  per-file pass rate (excluding documented exclusions):\n");
    for (size_t i = 0; i < grid.size(); ++i) {
        std::printf("%s%s", i % 8 == 0 ? "    " : "  ", grid[i].c_str());
        if (i % 8 == 7 || i + 1 == grid.size()) std::printf("\n");
    }
    std::printf("  %d files, %lld tests: %lld passed, %lld failed; excluded: %lld #GP(13), %lld revoked, %lld 1MB-alias\n",
                files, total, passed, failed, gp, revoked, alias);
    CHECK_EQ(failed, 0);
}
