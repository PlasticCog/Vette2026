#pragma once
// Real-mode address space for the hosted VETTE.EXE: 1 MB, wrapping at 1 MB (A20 disabled).
// A0000-AFFFF is EGA video memory and is routed to a VideoMemory device (planar, latched),
// everything else is plain RAM. The top 64 KB (F0000-FFFFF) holds the host BIOS stubs and is
// write-protected once the BIOS is installed.

#include <cstdint>
#include <vector>

namespace vette::host {

class VideoMemory {
public:
    virtual ~VideoMemory() = default;
    // `offset` is relative to A0000h (0..FFFFh).
    virtual uint8_t vram_read(uint32_t offset) = 0;
    virtual void vram_write(uint32_t offset, uint8_t value) = 0;
};

class Memory {
public:
    static constexpr uint32_t kSize = 0x100000;
    static constexpr uint32_t kMask = kSize - 1;
    static constexpr uint32_t kVideoBase = 0xA0000;
    static constexpr uint32_t kVideoSize = 0x10000;

    Memory() : ram_(kSize, 0) {}

    void set_video(VideoMemory* video) { video_ = video; }
    void protect_from(uint32_t linear) { rom_start_ = linear; }

    uint8_t read8(uint32_t linear) {
        linear &= kMask;
        if (linear - kVideoBase < kVideoSize && video_) {
            return video_->vram_read(linear - kVideoBase);
        }
        return ram_[linear];
    }

    void write8(uint32_t linear, uint8_t value) {
        linear &= kMask;
        if (linear - kVideoBase < kVideoSize && video_) {
            video_->vram_write(linear - kVideoBase, value);
        } else if (linear < rom_start_) {
            if (journal_) {
                journal_->push_back({linear, ram_[linear]});
            }
            ram_[linear] = value;
        }
    }

    // While set, every RAM write appends {address, previous value}. Used by the verification
    // harness to roll a function's effects back. Video memory writes are not journaled.
    struct JournalEntry {
        uint32_t linear;
        uint8_t old;
    };
    void set_journal(std::vector<JournalEntry>* journal) { journal_ = journal; }

    // Linear little-endian word. Segment-offset wrap (offset FFFFh) is the CPU's job.
    uint16_t read16(uint32_t linear) {
        return static_cast<uint16_t>(read8(linear) | (read8(linear + 1) << 8));
    }
    void write16(uint32_t linear, uint16_t value) {
        write8(linear, static_cast<uint8_t>(value));
        write8(linear + 1, static_cast<uint8_t>(value >> 8));
    }

    // Direct RAM access for the loader and HLE services. Bypasses video routing and ROM protection.
    uint8_t* ram() { return ram_.data(); }

private:
    std::vector<uint8_t> ram_;
    std::vector<JournalEntry>* journal_ = nullptr;
    VideoMemory* video_ = nullptr;
    uint32_t rom_start_ = kSize;
};

class IoBus {
public:
    virtual ~IoBus() = default;
    virtual uint8_t in8(uint16_t port) = 0;
    virtual void out8(uint16_t port, uint8_t value) = 0;
    // Word I/O is two byte accesses to port and port+1 (e.g. `out dx,ax` to 3CEh writes index then data).
    virtual uint16_t in16(uint16_t port) {
        return static_cast<uint16_t>(in8(port) | (in8(static_cast<uint16_t>(port + 1)) << 8));
    }
    virtual void out16(uint16_t port, uint16_t value) {
        out8(port, static_cast<uint8_t>(value));
        out8(static_cast<uint16_t>(port + 1), static_cast<uint8_t>(value >> 8));
    }
};

} // namespace vette::host
