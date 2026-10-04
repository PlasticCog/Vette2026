#pragma once
// High-level DOS 5.0 services (INT 20h/21h) and the EXE loader.
// Files resolve by name (drive and directories ignored, case-insensitive) in a writable save
// folder first, then in the read-only game folder. Opening a game file for writing copies it to the
// save folder first, so the player's original files are never modified.
// Memory management uses real MCB chains in emulated RAM, so allocations land where DOS puts them.

#include <array>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "host/bios.h"
#include "host/cpu.h"

namespace vette::host {

class Dos {
public:
    struct Paths {
        std::filesystem::path game_dir;
        std::filesystem::path save_dir;
    };

    Dos(Memory& mem, Bios& bios, Paths paths);
    ~Dos();
    Dos(const Dos&) = delete;
    Dos& operator=(const Dos&) = delete;

    // Loads an MZ executable like DOS EXEC: environment and PSP just below `load_seg`, the image at
    // `load_seg`, relocations applied, and CPU registers set for entry.
    bool load_exe(Cpu& cpu, const std::vector<uint8_t>& exe, uint16_t load_seg, const std::string& dos_path,
                  std::string& error);

    void int21(Cpu& cpu);
    void int20(Cpu& cpu);

    bool terminated() const { return terminated_; }
    int exit_code() const { return exit_code_; }

    void set_clock(std::function<RealTime()> clock) { clock_ = std::move(clock); }
    void set_log(std::function<void(const std::string&)> log) { log_ = std::move(log); }
    void set_console(std::function<void(char)> out) { console_ = std::move(out); }

private:
    struct OpenFile {
        std::FILE* f = nullptr;
        std::filesystem::path path;
    };
    static constexpr int kMaxHandles = 20;

    std::filesystem::path find(const std::filesystem::path& dir, const std::string& dos_name) const;
    std::filesystem::path resolve_read(const std::string& dos_path) const;
    std::filesystem::path resolve_write(const std::string& dos_path, bool create);
    int alloc_handle(std::FILE* f, std::filesystem::path path);

    void open(Cpu& cpu, bool create);
    void read(Cpu& cpu);
    void write(Cpu& cpu);
    void seek(Cpu& cpu);
    void console_input(Cpu& cpu, bool blocking, bool echo);

    // MCB arena
    struct Mcb {
        uint8_t type;
        uint16_t owner, size;
    };
    Mcb mcb(uint16_t seg);
    void set_mcb(uint16_t seg, Mcb m);
    void coalesce();
    void allocate(Cpu& cpu);
    void release(Cpu& cpu);
    void resize(Cpu& cpu);

    void fail(Cpu& cpu, uint16_t code);
    void ok(Cpu& cpu);
    void log_once(const std::string& what);

    Memory& mem_;
    Bios& bios_;
    Paths paths_;
    std::array<OpenFile, kMaxHandles> files_{};
    uint16_t first_mcb_ = 0;
    uint16_t psp_ = 0;
    uint16_t dta_seg_ = 0, dta_off_ = 0x80;
    int pending_scan_ = -1;  // second byte of an extended key for AH=06h/07h/08h
    uint16_t last_error_ = 0;
    bool terminated_ = false;
    int exit_code_ = 0;
    std::function<RealTime()> clock_;
    std::function<void(const std::string&)> log_;
    std::function<void(char)> console_;
    std::vector<std::string> logged_;
};

} // namespace vette::host
