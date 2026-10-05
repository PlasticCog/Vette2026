#pragma once
// Runs the original's object draw routines on a scratch copy of the machine and records what they
// hand to the renderer, instead of drawing it (internal to the world extraction and its validation).
//
// The renderer entry points are code hooks that record the call and return at once: the packed and
// plain vertex transforms, the polygon fills, face culling, line lists and the segment-245A model
// draws, plus the `call bx` of the two compound-structure list walkers. The matrix helpers the
// routines call themselves (3009:3917, 3DF9, 25CB, 9C7B) run normally and are watched.
//
// Two ways to run:
//  - isolated (extraction): every write of a run, including the state the caller sets up, is
//    journaled and rolled back afterwards, so each probe starts from the same machine state;
//  - sequential (validation): state carries over from run to run as it does in the original frame
//    (the compound guard, the finish-banner flag, animation counters, model colour patches).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "host/cpu.h"
#include "host/memory.h"

namespace vette::enhanced {

// Addresses in the unpacked image (DOS 1.1). Code offsets are in segment 3009, data in DS (124A).
namespace addr {
constexpr uint16_t kCodeSeg = 0x3009 + 0x1000;  // emulator segments: image + load segment 1000h
constexpr uint16_t kDataSeg = 0x124A + 0x1000;
constexpr uint16_t kModelSeg = 0x245A + 0x1000;

// Object state the routines read (notes 03 / 05).
constexpr uint16_t kObjPos = 0x3220;      // x, y, z of the object being drawn
constexpr uint16_t kObjAngles = 0x3226;   // yaw, pitch, roll for plain lists and models
constexpr uint16_t kCamera = 0x2C71;      // x, y, z, yaw, pitch, roll; +22h/+24h big tile row/col
constexpr uint16_t kCamPitchCopy = 0x2BEB;
constexpr uint16_t kFacing = 0x35C3;      // 0: camera faces +-x, 1: +-y
constexpr uint16_t kOwnCell = 0x35C4;
constexpr uint16_t kSortKey = 0x259E;     // in the code segment
constexpr uint16_t kWindowsOff = 0x2ABE;
constexpr uint16_t kCourse = 0x2CD5;
constexpr uint16_t kFinishFlag = 0x38D7;
constexpr uint16_t kCompoundDone = 0x2AC0;
constexpr uint16_t kMirrorPass = 0x18;
constexpr uint16_t kNoBuildings = 0x19;
constexpr uint16_t kViewOffset = 0x2B87;
constexpr uint16_t kHighway = 0x2AD4;
constexpr uint16_t kVertexCount = 0x3446;
constexpr uint16_t kFaceCount = 0x337D;
constexpr uint16_t kFaceTable = 0x337E;
constexpr uint16_t kFaceFlags = 0x333D;
constexpr uint16_t kLineList = 0x3269;
constexpr uint16_t kLineColour = 0x3337;
constexpr uint16_t kPlainMatrix = 0x331D;   // object x camera matrix for plain lists (25CB)
constexpr uint16_t kCameraPoint = 0x344E;   // 3917's result
constexpr uint16_t kModelMatrix = 0xE0B6;   // object x camera matrix for models, then translation
} // namespace addr

struct TraceEvent {
    enum class Op : uint8_t {
        Xform,    // packed block 3879: addr = block, count = [3446], pos = origin, data = packed words
        Plain,    // plain list 393F: addr = list, count = [3446], pos = 3917 point, data = list words
        Fill,     // B5B3/B5BC: addr = poly list, colour = AL, aux = BX, data = list words up to FFFF
        FillAlt,  // B63F/B636
        Vis5,     // 3B78: addr = face table, count = faces, data = table bytes
        Vis6,     // 3BF5
        Lines,    // 405E: addr = [3269], colour = [3337], count = n, data = n index words
        Model,    // B9BA/B9D6/B9E6: addr = model id, aux = entry (0 yaw, 1 full, 2 none), pos, angles
        SubCall,  // compound walker `call bx`: addr = routine, pos = position
        Bad,      // an unhooked renderer path ran: addr = where
    };
    Op op = Op::Bad;
    uint8_t colour = 0;
    uint16_t addr = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    std::array<int16_t, 3> pos{};     // relative to the run's entry position (16-bit wrap)
    std::array<int16_t, 3> angles{};  // Plain: from the last 3DF9 call; Model: [3226..322A]
    uint16_t angle_addr = 0;          // Plain: DS offset 3DF9 read the angles from (0: none this run)
    std::array<int16_t, 3> angle_prior{};  // Plain: the angle words as they were when the run started
    // Validation extras (exact transform as the original computed it).
    std::array<int16_t, 9> matrix{};  // Plain: [331D]; Model: [E0B6]
    std::array<int16_t, 3> translation{};  // Plain: [344E]; Model: [E0C8]
    uint16_t model_header = 0;        // Model: the header draw_model got (near or far mesh)
    uint8_t outline_enable = 0;       // Model: DS:E0D8 when drawn (face flag bit 13 outlines)
    uint32_t data_first = 0, data_count = 0;  // into Trace::data
};

struct Trace {
    std::vector<TraceEvent> events;
    std::vector<int16_t> data;
    std::string error;
    uint64_t signature() const;  // identity: everything but angles, matrices and validation extras
    void clear() {
        events.clear();
        data.clear();
        error.clear();
    }
};

class Tracer {
public:
    // `ram` is a 1 MB image of a machine whose VETTE.EXE is unpacked; it is copied.
    explicit Tracer(const uint8_t* ram);
    ~Tracer();
    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;

    void load(const uint8_t* ram);  // replace the scratch RAM (no rollback pending)
    host::Memory& memory();

    // Isolated mode: start journaling (the caller then writes its state), run, then end() rolls
    // everything back. Sequential mode: just run().
    void begin();
    void end();

    // Face hiding for dependency probes: in the `vis_call`-th culling call of the run, face `face`
    // is reported hidden. -1 = all faces visible.
    void hide_face(int vis_call, int face) {
        hide_call_ = vis_call;
        hide_face_ = face;
    }

    // Runs the near routine at code offset `routine` with DS:3220..3224 already holding the entry
    // position `entry`, recording into `out`. Returns false (out.error set) if it did not return.
    bool run(uint16_t routine, std::array<int16_t, 3> entry, Trace& out);

    // Calls the near routine `routine` (code segment 3009) with the general registers in `regs`
    // (DS = ES = the data segment), and returns them as the routine left them. Hooks stay active.
    bool call(uint16_t routine, host::Registers& regs);

    size_t runs() const { return runs_; }
    // Whether the current (journaled) run wrote the byte at seg:off.
    bool wrote(uint16_t seg, uint16_t off) const;

    // Validation: let the model entry points run (object matrix, near/far choice, far colour patch)
    // and record draw_model's inputs (TraceEvent::matrix, translation, model_header) instead.
    void set_exact_models(bool on) { exact_models_ = on; }

    // Data helpers on the scratch memory.
    uint16_t rd16(uint16_t seg, uint16_t off);
    uint8_t rd8(uint16_t seg, uint16_t off);
    void wr16(uint16_t seg, uint16_t off, uint16_t v);
    void wr8(uint16_t seg, uint16_t off, uint8_t v);

private:
    class NullIo;
    void install_hooks();
    void ret_near();
    void record(TraceEvent ev, const int16_t* data = nullptr, size_t n = 0);
    void bad(uint16_t where);

    std::unique_ptr<NullIo> io_;
    host::Memory mem_;
    std::unique_ptr<host::Cpu> cpu_;
    std::vector<host::Memory::JournalEntry> journal_;
    bool journaling_ = false;

    Trace* cur_ = nullptr;
    std::array<int16_t, 3> entry_{};
    bool done_ = false;
    int vis_calls_ = 0;
    int hide_call_ = -1, hide_face_ = -1;
    bool exact_models_ = false;
    // Watched helper state of the current run.
    std::array<int16_t, 3> last_point_{};
    uint16_t angle_addr_ = 0;
    std::array<int16_t, 3> angles_{};
    std::array<int16_t, 3> angle_prior_{};
    size_t journal_mark_ = 0;  // journal size when the current run started
    size_t runs_ = 0;
};

// FNV-1a, 64-bit.
struct Hash64 {
    uint64_t h = 0xCBF29CE484222325ull;
    void byte(uint8_t b) {
        h ^= b;
        h *= 0x100000001B3ull;
    }
    void word(uint16_t w) {
        byte(static_cast<uint8_t>(w));
        byte(static_cast<uint8_t>(w >> 8));
    }
};

} // namespace vette::enhanced
