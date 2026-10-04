#pragma once
// IBM EGA (256 KB) as used by VETTE!: planar graphics modes 0Dh (320x200x16) and 0Eh (640x200x16),
// with latches, sequencer map mask, all graphics-controller functions, the attribute-controller
// palette, CRTC start address (page flipping: VETTE draws to pages at A000:0000 and A000:4000), and
// the input status register's retrace bits for code that waits on vertical retrace.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "host/memory.h"

namespace vette::host {

class Ega final : public VideoMemory {
public:
    struct Frame {
        int width = 0;   // 640 or 320 (0 in text modes, which are not rendered)
        int height = 0;  // 200
        std::vector<uint8_t> pixels;            // palette indices 0..15, row-major
        std::array<uint32_t, 16> palette{};     // 0xRRGGBB for each index, via the attribute controller
    };

    Ega();
    ~Ega() override;
    Ega(const Ega&) = delete;
    Ega& operator=(const Ega&) = delete;

    // VideoMemory: CPU accesses to A0000h-AFFFFh.
    uint8_t vram_read(uint32_t offset) override;
    void vram_write(uint32_t offset, uint8_t value) override;

    // I/O: 3C0h-3CFh, 3D4h/3D5h, 3DAh. handles() says whether a port belongs to the EGA.
    static bool handles(uint16_t port);
    uint8_t in8(uint16_t port);
    void out8(uint16_t port, uint8_t value);

    // BIOS services (INT 10h). Mode set programs registers like the EGA BIOS and clears video memory
    // unless `clear` is false. Text modes (00h-03h, 07h) are accepted but not rendered.
    void set_mode(uint8_t mode, bool clear = true);
    uint8_t mode() const;
    void set_palette_register(uint8_t index, uint8_t ega_color);  // INT 10h AX=1000h
    void set_border(uint8_t ega_color);                           // INT 10h AX=1001h

    // Emulated time in nanoseconds, used for the 3DAh retrace and display-enable bits (~60 Hz,
    // 262 lines) and for the CRTC's start-address latch (a new start address is displayed from the
    // next vertical retrace on). Defaults to a source that always returns 0; without a source, a new
    // start address is displayed immediately.
    void set_time_source(std::function<uint64_t()> now_ns);

    // The currently displayed page (CRTC start address) as indices plus the active palette.
    void render(Frame& out) const;
    // The same from an explicit start address, e.g. a back-buffer page the program is drawing into.
    void render_page(uint16_t start_address, Frame& out) const;
    // The start address the display shows now (the CRTC start as latched at vertical retrace).
    uint16_t display_start() const;
    // Vertical retrace timing without the side effect of reading 3DAh (which resets the attribute
    // flip-flop): whether a retrace is in progress now, and the emulated time of the next retrace
    // start (or end) strictly after now. Used to skip time a program spends polling for retrace.
    bool in_vertical_retrace() const;
    uint64_t next_vertical_retrace_ns(bool start) const;

    // Verification harness: copy the complete device state (registers, latches, video memory) from
    // another Ega, keeping this one's time source; and describe how two states differ (empty if
    // identical), listing at most `max_items` differences.
    void copy_state_from(const Ega& other);
    std::string diff_state(const Ega& other, size_t max_items = 8) const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace vette::host
