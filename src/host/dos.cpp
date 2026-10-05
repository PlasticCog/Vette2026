#include "host/dos.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <system_error>

#include "host/hle.h"

namespace vette::host {
namespace {

constexpr uint16_t kTopOfMemory = 0xA000;  // 640 KB
constexpr uint16_t kEnvParas = 0x10;
constexpr uint8_t kMcbMid = 'M', kMcbLast = 'Z';

enum DosError : uint16_t {
    kFunctionInvalid = 1, kFileNotFound = 2, kTooManyFiles = 4, kAccessDenied = 5, kBadHandle = 6,
    kMcbDestroyed = 7, kNoMemory = 8, kBadBlock = 9,
};

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// "C:\VETTE\TITLE.BIN" -> "TITLE.BIN"
std::string base_name(const std::string& dos_path) {
    const size_t cut = dos_path.find_last_of("\\/:");
    return upper(cut == std::string::npos ? dos_path : dos_path.substr(cut + 1));
}

std::FILE* open_file(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::FILE* f = nullptr;
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen_s(&f, p.c_str(), wmode.c_str()) == 0 ? f : nullptr;
#else
    return std::fopen(p.c_str(), mode);
#endif
}

int weekday(int y, int m, int d) {  // 0 = Sunday (Sakamoto)
    static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

} // namespace

Dos::Dos(Memory& mem, Bios& bios, Paths paths) : mem_(mem), bios_(bios), paths_(std::move(paths)) {}

Dos::~Dos() {
    for (OpenFile& of : files_) {
        if (of.f) {
            std::fclose(of.f);
        }
    }
}

void Dos::log_once(const std::string& what) {
    if (log_ && std::find(logged_.begin(), logged_.end(), what) == logged_.end()) {
        logged_.push_back(what);
        log_(what);
    }
}

void Dos::fail(Cpu& cpu, uint16_t code) {
    last_error_ = code;
    cpu.regs.r[AX] = code;
    hle::set_carry(cpu, true);
}

void Dos::ok(Cpu& cpu) { hle::set_carry(cpu, false); }

// --- Loader -------------------------------------------------------------------------------------

bool Dos::load_exe(Cpu& cpu, const std::vector<uint8_t>& exe, uint16_t load_seg, const std::string& dos_path,
                   std::string& error) {
    auto u16 = [&](size_t off) { return static_cast<uint16_t>(exe[off] | (exe[off + 1] << 8)); };
    if (exe.size() < 0x1C || u16(0) != 0x5A4D) {
        error = "not an MZ executable";
        return false;
    }
    const uint16_t last_page = u16(2), pages = u16(4), nrelocs = u16(6), hdr_paras = u16(8);
    const uint16_t min_alloc = u16(10), init_ss = u16(14), init_sp = u16(16), init_ip = u16(20);
    const uint16_t init_cs = u16(22), reloc_off = u16(24);
    const size_t file_len = pages * 512u - (last_page ? 512u - last_page : 0u);
    const size_t hdr_len = hdr_paras * 16u;
    if (file_len > exe.size() || hdr_len > file_len) {
        error = "truncated executable";
        return false;
    }
    const size_t image_len = file_len - hdr_len;

    psp_ = static_cast<uint16_t>(load_seg - 0x10);
    const uint16_t prog_mcb = static_cast<uint16_t>(psp_ - 1);
    const uint16_t env_seg = static_cast<uint16_t>(prog_mcb - kEnvParas);
    first_mcb_ = static_cast<uint16_t>(env_seg - 1);
    const uint32_t needed = static_cast<uint32_t>((image_len + 15) / 16) + min_alloc;
    if (load_seg + needed > kTopOfMemory) {
        error = "not enough conventional memory";
        return false;
    }

    // MCB chain: environment block, then the program block owning all remaining memory.
    set_mcb(first_mcb_, {kMcbMid, psp_, kEnvParas});
    set_mcb(prog_mcb, {kMcbLast, psp_, static_cast<uint16_t>(kTopOfMemory - psp_)});

    // Environment: variables, a double NUL, a count word, then the program's full path.
    uint8_t* ram = mem_.ram();
    std::memset(ram + env_seg * 16u, 0, kEnvParas * 16u);
    const std::string env = std::string("PATH=C:\\") + '\0' + "COMSPEC=C:\\COMMAND.COM" + '\0' + '\0' +
                            '\x01' + '\0' + dos_path + '\0';
    std::memcpy(ram + env_seg * 16u, env.data(), std::min<size_t>(env.size(), kEnvParas * 16u));

    // PSP.
    uint8_t* psp = ram + psp_ * 16u;
    std::memset(psp, 0, 0x100);
    psp[0] = 0xCD;
    psp[1] = 0x20;
    mem_.write16(psp_ * 16u + 0x02, kTopOfMemory);
    mem_.write16(psp_ * 16u + 0x0A, mem_.read16(0x22 * 4));  // saved INT 22h/23h/24h
    mem_.write16(psp_ * 16u + 0x0C, mem_.read16(0x22 * 4 + 2));
    mem_.write16(psp_ * 16u + 0x0E, mem_.read16(0x23 * 4));
    mem_.write16(psp_ * 16u + 0x10, mem_.read16(0x23 * 4 + 2));
    mem_.write16(psp_ * 16u + 0x12, mem_.read16(0x24 * 4));
    mem_.write16(psp_ * 16u + 0x14, mem_.read16(0x24 * 4 + 2));
    mem_.write16(psp_ * 16u + 0x16, psp_);
    const uint8_t jft[kMaxHandles] = {1, 1, 1, 0, 2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::memcpy(psp + 0x18, jft, sizeof jft);
    mem_.write16(psp_ * 16u + 0x2C, env_seg);
    mem_.write16(psp_ * 16u + 0x32, kMaxHandles);
    mem_.write16(psp_ * 16u + 0x34, 0x18);
    mem_.write16(psp_ * 16u + 0x36, psp_);
    psp[0x50] = 0xCD;
    psp[0x51] = 0x21;
    psp[0x52] = 0xCB;
    std::memset(psp + 0x5D, ' ', 11);
    std::memset(psp + 0x6D, ' ', 11);
    psp[0x80] = 0;
    psp[0x81] = 0x0D;

    // Image and relocations.
    std::memcpy(ram + load_seg * 16u, exe.data() + hdr_len, image_len);
    for (uint16_t i = 0; i < nrelocs; ++i) {
        const size_t at = reloc_off + i * 4u;
        if (at + 4 > exe.size()) {
            error = "bad relocation table";
            return false;
        }
        const uint32_t addr = Cpu::linear(static_cast<uint16_t>(load_seg + u16(at + 2)), u16(at));
        mem_.write16(addr, static_cast<uint16_t>(mem_.read16(addr) + load_seg));
    }

    Registers& r = cpu.regs;
    r = Registers{};
    r.s[CS] = static_cast<uint16_t>(load_seg + init_cs);
    r.ip = init_ip;
    r.s[SS] = static_cast<uint16_t>(load_seg + init_ss);
    r.r[SP] = init_sp;
    r.s[DS] = r.s[ES] = psp_;
    r.r[CX] = 0x00FF;
    r.r[DX] = psp_;
    r.flags = 0x0202;
    dta_seg_ = psp_;
    dta_off_ = 0x80;
    files_ = {};
    terminated_ = false;
    return true;
}

// --- Files --------------------------------------------------------------------------------------

std::filesystem::path Dos::find(const std::filesystem::path& dir, const std::string& dos_name) const {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && upper(entry.path().filename().string()) == dos_name) {
            return entry.path();
        }
    }
    return {};
}

std::filesystem::path Dos::resolve_read(const std::string& dos_path) const {
    const std::string name = base_name(dos_path);
    std::filesystem::path p = find(paths_.save_dir, name);
    return p.empty() ? find(paths_.game_dir, name) : p;
}

std::filesystem::path Dos::resolve_write(const std::string& dos_path, bool create) {
    const std::string name = base_name(dos_path);
    if (std::filesystem::path p = find(paths_.save_dir, name); !p.empty()) {
        return p;
    }
    std::error_code ec;
    std::filesystem::create_directories(paths_.save_dir, ec);
    const std::filesystem::path target = paths_.save_dir / name;
    if (!create) {
        const std::filesystem::path original = find(paths_.game_dir, name);
        if (original.empty()) {
            return {};
        }
        std::filesystem::copy_file(original, target, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return {};
        }
    }
    return target;
}

int Dos::alloc_handle(std::FILE* f, std::filesystem::path path) {
    for (int h = 5; h < kMaxHandles; ++h) {
        if (!files_[static_cast<size_t>(h)].f) {
            files_[static_cast<size_t>(h)] = {f, std::move(path)};
            return h;
        }
    }
    std::fclose(f);
    return -1;
}

void Dos::open(Cpu& cpu, bool create) {
    Registers& r = cpu.regs;
    const std::string name = hle::read_asciiz(mem_, r.s[DS], r.r[DX]);
    const int access = r.lo(AX) & 7;
    std::filesystem::path p;
    const char* mode = "rb";
    if (create) {
        p = resolve_write(name, true);
        mode = "w+b";
    } else if (access == 0) {
        p = resolve_read(name);
    } else {
        p = resolve_write(name, false);
        mode = "r+b";
    }
    std::FILE* f = p.empty() ? nullptr : open_file(p, mode);
    if (log_) {
        log_(std::string(create ? "create " : "open ") + name + (f ? "" : " (not found)"));
    }
    if (!f) {
        fail(cpu, create ? kAccessDenied : kFileNotFound);
        return;
    }
    const int h = alloc_handle(f, p);
    if (h < 0) {
        fail(cpu, kTooManyFiles);
        return;
    }
    r.r[AX] = static_cast<uint16_t>(h);
    ok(cpu);
}

void Dos::read(Cpu& cpu) {
    Registers& r = cpu.regs;
    const uint16_t h = r.r[BX];
    if (h < 5) {
        r.r[AX] = 0;  // stdin/aux/prn: nothing to read
        ok(cpu);
        return;
    }
    if (h >= kMaxHandles || !files_[h].f) {
        fail(cpu, kBadHandle);
        return;
    }
    std::vector<uint8_t> buf(r.r[CX]);
    const size_t n = std::fread(buf.data(), 1, buf.size(), files_[h].f);
    for (size_t i = 0; i < n; ++i) {
        mem_.write8(Cpu::linear(r.s[DS], static_cast<uint16_t>(r.r[DX] + i)), buf[i]);
    }
    r.r[AX] = static_cast<uint16_t>(n);
    ok(cpu);
}

void Dos::write(Cpu& cpu) {
    Registers& r = cpu.regs;
    const uint16_t h = r.r[BX];
    std::vector<uint8_t> buf(r.r[CX]);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = mem_.read8(Cpu::linear(r.s[DS], static_cast<uint16_t>(r.r[DX] + i)));
    }
    if (h == 1 || h == 2) {
        for (uint8_t c : buf) {
            if (console_) {
                console_(static_cast<char>(c));
            }
        }
        r.r[AX] = r.r[CX];
        ok(cpu);
        return;
    }
    if (h >= kMaxHandles || !files_[h].f) {
        fail(cpu, kBadHandle);
        return;
    }
    OpenFile& of = files_[h];
    if (buf.empty()) {  // CX=0 truncates or extends the file to the current position
        std::fflush(of.f);
        const long pos = std::ftell(of.f);
        std::error_code ec;
        std::filesystem::resize_file(of.path, static_cast<uintmax_t>(pos), ec);
        r.r[AX] = 0;
        ok(cpu);
        return;
    }
    r.r[AX] = static_cast<uint16_t>(std::fwrite(buf.data(), 1, buf.size(), of.f));
    std::fflush(of.f);
    ok(cpu);
}

void Dos::seek(Cpu& cpu) {
    Registers& r = cpu.regs;
    const uint16_t h = r.r[BX];
    if (h >= kMaxHandles || !files_[h].f) {
        fail(cpu, kBadHandle);
        return;
    }
    const auto offset = static_cast<int32_t>(static_cast<uint32_t>(r.r[CX]) << 16 | r.r[DX]);
    const int origin = r.lo(AX) == 0 ? SEEK_SET : r.lo(AX) == 1 ? SEEK_CUR : SEEK_END;
    if (std::fseek(files_[h].f, offset, origin) != 0) {
        fail(cpu, kFunctionInvalid);
        return;
    }
    const auto pos = static_cast<uint32_t>(std::ftell(files_[h].f));
    r.r[AX] = static_cast<uint16_t>(pos);
    r.r[DX] = static_cast<uint16_t>(pos >> 16);
    ok(cpu);
}

void Dos::console_input(Cpu& cpu, bool blocking, bool echo) {
    Registers& r = cpu.regs;
    if (pending_scan_ >= 0) {
        r.set_lo(AX, static_cast<uint8_t>(pending_scan_));
        pending_scan_ = -1;
        hle::set_return_flag(cpu, flag::ZF, false);
        return;
    }
    const int key = bios_.read_key(true);
    if (key < 0) {
        if (blocking) {
            hle::retry_int(cpu);
        } else {
            r.set_lo(AX, 0);
            hle::set_return_flag(cpu, flag::ZF, true);
        }
        return;
    }
    const auto ascii = static_cast<uint8_t>(key);
    if (ascii == 0 || ascii == 0xE0) {
        pending_scan_ = key >> 8;  // extended key: 00h now, scan code on the next call
    }
    r.set_lo(AX, ascii == 0xE0 ? 0 : ascii);
    hle::set_return_flag(cpu, flag::ZF, false);
    if (echo && console_ && ascii) {
        console_(static_cast<char>(ascii));
    }
}

// --- Memory -------------------------------------------------------------------------------------

Dos::Mcb Dos::mcb(uint16_t seg) {
    const uint32_t a = seg * 16u;
    return {mem_.read8(a), mem_.read16(a + 1), mem_.read16(a + 3)};
}

void Dos::set_mcb(uint16_t seg, Mcb m) {
    const uint32_t a = seg * 16u;
    mem_.write8(a, m.type);
    mem_.write16(a + 1, m.owner);
    mem_.write16(a + 3, m.size);
}

void Dos::coalesce() {
    uint16_t seg = first_mcb_;
    for (;;) {
        Mcb m = mcb(seg);
        if (m.type == kMcbLast) {
            return;
        }
        const auto next_seg = static_cast<uint16_t>(seg + m.size + 1);
        const Mcb next = mcb(next_seg);
        if (m.owner == 0 && next.owner == 0) {
            m.size = static_cast<uint16_t>(m.size + next.size + 1);
            m.type = next.type;
            set_mcb(seg, m);
            continue;  // try to absorb the following block too
        }
        seg = next_seg;
    }
}

void Dos::allocate(Cpu& cpu) {
    Registers& r = cpu.regs;
    const uint16_t want = r.r[BX];
    coalesce();
    uint16_t largest = 0;
    for (uint16_t seg = first_mcb_;;) {
        Mcb m = mcb(seg);
        if (m.type != kMcbMid && m.type != kMcbLast) {
            fail(cpu, kMcbDestroyed);
            return;
        }
        if (m.owner == 0) {
            if (m.size >= want) {
                if (m.size > want) {
                    set_mcb(static_cast<uint16_t>(seg + want + 1), {m.type, 0, static_cast<uint16_t>(m.size - want - 1)});
                    m.type = kMcbMid;
                }
                m.size = want;
                m.owner = psp_;
                set_mcb(seg, m);
                r.r[AX] = static_cast<uint16_t>(seg + 1);
                ok(cpu);
                return;
            }
            largest = std::max(largest, m.size);
        }
        if (m.type == kMcbLast) {
            break;
        }
        seg = static_cast<uint16_t>(seg + m.size + 1);
    }
    fail(cpu, kNoMemory);
    r.r[BX] = largest;
}

void Dos::release(Cpu& cpu) {
    const auto seg = static_cast<uint16_t>(cpu.regs.s[ES] - 1);
    Mcb m = mcb(seg);
    if (m.type != kMcbMid && m.type != kMcbLast) {
        fail(cpu, kBadBlock);
        return;
    }
    m.owner = 0;
    set_mcb(seg, m);
    ok(cpu);
}

void Dos::resize(Cpu& cpu) {
    Registers& r = cpu.regs;
    const auto seg = static_cast<uint16_t>(r.s[ES] - 1);
    const uint16_t want = r.r[BX];
    Mcb m = mcb(seg);
    if (m.type != kMcbMid && m.type != kMcbLast) {
        fail(cpu, kBadBlock);
        return;
    }
    // Absorb following free blocks so growing can succeed.
    while (m.type != kMcbLast && m.size < want) {
        const Mcb next = mcb(static_cast<uint16_t>(seg + m.size + 1));
        if (next.owner != 0) {
            break;
        }
        m.size = static_cast<uint16_t>(m.size + next.size + 1);
        m.type = next.type;
    }
    if (m.size < want) {
        set_mcb(seg, m);
        fail(cpu, kNoMemory);
        r.r[BX] = m.size;
        return;
    }
    if (m.size > want) {
        set_mcb(static_cast<uint16_t>(seg + want + 1), {m.type, 0, static_cast<uint16_t>(m.size - want - 1)});
        m.type = kMcbMid;
        m.size = want;
    }
    set_mcb(seg, m);
    ok(cpu);
}

// --- Dispatch -----------------------------------------------------------------------------------

void Dos::int20(Cpu& cpu) {
    terminated_ = true;
    exit_code_ = 0;
    cpu.request_stop();
}

void Dos::int21(Cpu& cpu) {
    Registers& r = cpu.regs;
    const uint8_t ah = r.hi(AX);
    const RealTime now = clock_ ? clock_() : RealTime{1989, 10, 23, 12, 0, 0, 0};
    switch (ah) {
    case 0x00:
        int20(cpu);
        break;
    case 0x01:
        console_input(cpu, true, true);
        break;
    case 0x02:
        if (console_) {
            console_(static_cast<char>(r.lo(DX)));
        }
        break;
    case 0x06:
        if (r.lo(DX) == 0xFF) {
            console_input(cpu, false, false);
        } else if (console_) {
            console_(static_cast<char>(r.lo(DX)));
        }
        break;
    case 0x07:
    case 0x08:
        console_input(cpu, true, false);
        break;
    case 0x09:
        for (uint16_t off = r.r[DX];; ++off) {
            const char c = static_cast<char>(mem_.read8(Cpu::linear(r.s[DS], off)));
            if (c == '$') {
                break;
            }
            if (console_) {
                console_(c);
            }
        }
        break;
    case 0x0B:
        r.set_lo(AX, bios_.read_key(false) >= 0 || pending_scan_ >= 0 ? 0xFF : 0x00);
        break;
    case 0x0C: {
        while (bios_.read_key(true) >= 0) {
        }
        pending_scan_ = -1;
        const uint8_t fn = r.lo(AX);
        if (fn == 0x01 || fn == 0x06 || fn == 0x07 || fn == 0x08) {
            // AH stays as the input function: if it blocks and the INT is retried, the retry must
            // not flush again and discard the key it is waiting for.
            r.set_hi(AX, fn);
            int21(cpu);
        }
        break;
    }
    case 0x0E:
        r.set_lo(AX, 5);
        break;
    case 0x19:
        r.set_lo(AX, 2);  // C:
        break;
    case 0x1A:
        dta_seg_ = r.s[DS];
        dta_off_ = r.r[DX];
        break;
    case 0x25:
        mem_.write16(r.lo(AX) * 4u, r.r[DX]);
        mem_.write16(r.lo(AX) * 4u + 2, r.s[DS]);
        break;
    case 0x2A:
        r.r[CX] = static_cast<uint16_t>(now.year);
        r.set_hi(DX, static_cast<uint8_t>(now.month));
        r.set_lo(DX, static_cast<uint8_t>(now.day));
        r.set_lo(AX, static_cast<uint8_t>(weekday(now.year, now.month, now.day)));
        break;
    case 0x2C:
        r.set_hi(CX, static_cast<uint8_t>(now.hour));
        r.set_lo(CX, static_cast<uint8_t>(now.minute));
        r.set_hi(DX, static_cast<uint8_t>(now.second));
        r.set_lo(DX, static_cast<uint8_t>(now.hundredths));
        break;
    case 0x2F:
        r.s[ES] = dta_seg_;
        r.r[BX] = dta_off_;
        break;
    case 0x30:
        r.r[AX] = 0x0005;  // DOS 5.0
        r.r[BX] = r.r[CX] = 0;
        break;
    case 0x33:
        if (r.lo(AX) == 0x00) {
            r.set_lo(DX, 0);
        } else if (r.lo(AX) == 0x05) {
            r.set_lo(DX, 3);
        }
        break;
    case 0x35:
        r.r[BX] = mem_.read16(r.lo(AX) * 4u);
        r.s[ES] = mem_.read16(r.lo(AX) * 4u + 2);
        break;
    case 0x3B:
        ok(cpu);
        break;
    case 0x3C:
        open(cpu, true);
        break;
    case 0x3D:
        open(cpu, false);
        break;
    case 0x3E: {
        const uint16_t h = r.r[BX];
        if (h < 5) {
            ok(cpu);
        } else if (h < kMaxHandles && files_[h].f) {
            std::fclose(files_[h].f);
            files_[h] = {};
            ok(cpu);
        } else {
            fail(cpu, kBadHandle);
        }
        break;
    }
    case 0x3F:
        read(cpu);
        break;
    case 0x40:
        write(cpu);
        break;
    case 0x41:
        fail(cpu, kAccessDenied);
        log_once("delete refused: " + hle::read_asciiz(mem_, r.s[DS], r.r[DX]));
        break;
    case 0x42:
        seek(cpu);
        break;
    case 0x43:
        if (r.lo(AX) == 0x00 && resolve_read(hle::read_asciiz(mem_, r.s[DS], r.r[DX])).empty()) {
            fail(cpu, kFileNotFound);
        } else {
            r.r[CX] = 0x20;
            ok(cpu);
        }
        break;
    case 0x44:
        if (r.lo(AX) == 0x00) {
            const uint16_t h = r.r[BX];
            r.r[DX] = h < 5 ? 0x80D3 : 0x0002;  // character device / file on drive C:
            ok(cpu);
        } else if (r.lo(AX) == 0x08) {
            r.r[AX] = 1;  // fixed disk
            ok(cpu);
        } else {
            ok(cpu);
        }
        break;
    case 0x47:
        mem_.write8(Cpu::linear(r.s[DS], r.r[SI]), 0);  // root directory
        ok(cpu);
        break;
    case 0x48:
        allocate(cpu);
        break;
    case 0x49:
        release(cpu);
        break;
    case 0x4A:
        resize(cpu);
        break;
    case 0x4C:
        terminated_ = true;
        exit_code_ = r.lo(AX);
        cpu.request_stop();
        break;
    case 0x4D:
        r.r[AX] = static_cast<uint16_t>(exit_code_);
        break;
    case 0x50:
        psp_ = r.r[BX];
        break;
    case 0x51:
    case 0x62:
        r.r[BX] = psp_;
        break;
    case 0x58:
        if (r.lo(AX) == 0x00) {
            r.r[AX] = 0;
        }
        ok(cpu);
        break;
    case 0x59:
        r.r[AX] = last_error_;
        break;
    default:
        log_once("unhandled INT 21h AH=" + std::to_string(ah));
        fail(cpu, kFunctionInvalid);
        break;
    }
}

} // namespace vette::host
