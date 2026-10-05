#include "assets/pict.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace vette::assets {

long unpack_bits(std::span<const std::uint8_t> in, std::uint8_t* out, std::size_t out_size, int unit) {
    const std::size_t u = unit == 2 ? 2 : 1;
    std::size_t i = 0, o = 0;
    while (o < out_size) {
        if (i >= in.size()) return -1;
        const auto n = static_cast<std::int8_t>(in[i++]);
        if (n >= 0) {  // n+1 literal units
            const std::size_t count = (static_cast<std::size_t>(n) + 1) * u;
            if (in.size() - i < count) return -1;
            const std::size_t take = std::min(count, out_size - o);
            std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(i), take, out + o);
            i += count;
            o += take;
        } else if (n != -128) {  // one unit repeated 1-n times; -128 is a no-op
            if (in.size() - i < u) return -1;
            for (int r = 0; r < 1 - n && o < out_size; ++r)
                for (std::size_t k = 0; k < u && o < out_size; ++k) out[o++] = in[i + k];
            i += u;
        }
    }
    return static_cast<long>(i);
}

namespace {

constexpr int kMaxSide = 8192;                  // larger frames or bitmaps are refused
constexpr std::size_t kMaxArea = 32u << 20;     // pixels

struct Rect {
    int top = 0, left = 0, bottom = 0, right = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool empty() const { return right <= left || bottom <= top; }
};

Rect intersect(const Rect& a, const Rect& b) {
    return {std::max(a.top, b.top), std::max(a.left, b.left), std::min(a.bottom, b.bottom),
            std::min(a.right, b.right)};
}

struct Reader {
    std::span<const std::uint8_t> d;
    std::size_t p = 0;
    bool ok = true;

    bool need(std::size_t n) {
        if (!ok || p > d.size() || d.size() - p < n) ok = false;
        return ok;
    }
    std::uint8_t u8() { return need(1) ? d[p++] : 0; }
    std::uint16_t u16() {
        if (!need(2)) return 0;
        const auto v = static_cast<std::uint16_t>(d[p] << 8 | d[p + 1]);
        p += 2;
        return v;
    }
    std::int16_t s16() { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() {
        const std::uint32_t hi = u16();
        return hi << 16 | u16();
    }
    void skip(std::size_t n) {
        if (need(n)) p += n;
    }
    std::span<const std::uint8_t> bytes(std::size_t n) {
        if (!need(n)) return {};
        const auto s = d.subspan(p, n);
        p += n;
        return s;
    }
    Rect rect() {
        Rect r;
        r.top = s16();
        r.left = s16();
        r.bottom = s16();
        r.right = s16();
        return r;
    }
};

std::uint32_t rgb48(std::uint16_t r, std::uint16_t g, std::uint16_t b) {
    return static_cast<std::uint32_t>(r >> 8) << 16 | static_cast<std::uint32_t>(g >> 8) << 8 | (b >> 8);
}

// The classic QuickDraw colour constants (FgColor/BkColor opcodes).
std::uint32_t old_color(std::uint32_t c) {
    switch (c) {
    case 30: return 0xFFFFFF;   // whiteColor
    case 205: return 0xDD0806;  // redColor
    case 341: return 0x1FB714;  // greenColor
    case 409: return 0x0000D4;  // blueColor
    case 273: return 0x02ABEA;  // cyanColor
    case 137: return 0xF20884;  // magentaColor
    case 69: return 0xFCF305;   // yellowColor
    default: return 0x000000;   // blackColor (33) and anything unknown
    }
}

using Pattern = std::array<std::uint8_t, 8>;
constexpr Pattern kBlack = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr Pattern kWhite = {};

enum class Shape { Rect, RRect, Oval };

class Player {
public:
    Player(std::span<const std::uint8_t> data, Pict& out) : r_{data}, out_(out) {}

    bool run(std::string& error);

private:
    // Coordinates: picture space -> canvas.
    int cx(int h) const { return h - frame_.left; }
    int cy(int v) const { return v - frame_.top; }

    bool clipped(int x, int y) const {  // canvas coordinates
        if (x < clip_.left || x >= clip_.right || y < clip_.top || y >= clip_.bottom) return true;
        return !clip_mask_.empty() && !clip_mask_[static_cast<std::size_t>(y) * out_.width + x];
    }
    void plot(int x, int y, std::uint32_t rgb) {
        if (!clipped(x, y)) out_.pixels[static_cast<std::size_t>(y) * out_.width + x] = 0xFF000000u | rgb;
    }
    // A pattern pixel under a pattern-mode pen: patCopy draws both colours, patOr only the set bits,
    // patBic clears the set bits to the background colour.
    void pat_plot(int x, int y, const Pattern& pat, int mode) {
        const bool bit = (pat[static_cast<std::size_t>(y & 7)] >> (7 - (x & 7))) & 1;
        switch (mode & 0x3F) {
        case 9:   // patOr
        case 10:  // patXor (approximated)
            if (bit) plot(x, y, fg_);
            break;
        case 11:  // patBic
            if (bit) plot(x, y, bk_);
            break;
        default:  // patCopy and the rest
            plot(x, y, bit ? fg_ : bk_);
            break;
        }
    }
    void fill_canvas_rect(int x0, int y0, int x1, int y1, const Pattern& pat, int mode) {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, out_.width);
        y1 = std::min(y1, out_.height);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) pat_plot(x, y, pat, mode);
    }

    void op_line(int x0, int y0, int x1, int y1);
    void op_shape(Shape shape, int verb, const Rect& r);
    void op_poly(int verb);
    void op_region(int verb);
    bool op_bits(std::uint16_t op, std::string& error);
    bool skip_pixpat(std::string& error);
    bool read_region(Rect& bounds, std::vector<std::uint8_t>* mask, int mask_w, int mask_h, int dx, int dy);
    void text(int dh, int dv, bool absolute);

    // Reads `rows` rows of pixel data (packed unless rowBytes < 8 or `packed` is false) into `buf`,
    // `unpacked_row` bytes per row.
    bool read_rows(int rows, int row_bytes, std::size_t unpacked_row, bool packed, int unit,
                   std::vector<std::uint8_t>& buf);

    Reader r_;
    Pict& out_;
    bool v2_ = false;
    Rect frame_;
    Rect clip_;                             // canvas coordinates
    std::vector<std::uint8_t> clip_mask_;   // non-rectangular clip, canvas-sized (empty: rect only)
    std::uint32_t fg_ = 0x000000, bk_ = 0xFFFFFF;
    Pattern pen_pat_ = kBlack, fill_pat_ = kBlack, bk_pat_ = kWhite;
    int pen_w_ = 1, pen_h_ = 1, pen_mode_ = 8;
    int pen_x_ = 0, pen_y_ = 0;             // picture coordinates
    int text_x_ = 0, text_y_ = 0;
    int font_ = 0, size_ = 0, face_ = 0;
    int oval_w_ = 0, oval_h_ = 0;
    Rect last_rect_[4];                     // per shape kind (rect, rrect, oval, arc) for the "same" ops
};

void Player::op_line(int x0, int y0, int x1, int y1) {
    // The pen is a pen_w x pen_h rectangle hanging below and right of each point on the line.
    x0 = cx(x0);
    y0 = cy(y0);
    x1 = cx(x1);
    y1 = cy(y1);
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 4 * kMaxSide; ++guard) {
        fill_canvas_rect(x0, y0, x0 + pen_w_, y0 + pen_h_, pen_pat_, pen_mode_);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// Whether canvas pixel (x, y) lies inside a shape with corner ovals of ow x oh.
bool inside(Shape shape, const Rect& r, int ow, int oh, int x, int y) {
    if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) return false;
    if (shape == Shape::Rect) return true;
    if (shape == Shape::Oval) {
        ow = r.width();
        oh = r.height();
    }
    ow = std::min(ow, r.width());
    oh = std::min(oh, r.height());
    if (ow <= 1 || oh <= 1) return true;
    // Distance into the nearest corner box, measured from the corner ellipse's centre.
    const double rx = ow / 2.0, ry = oh / 2.0;
    const double px = x + 0.5, py = y + 0.5;
    double ex = 0, ey = 0;
    if (px < r.left + rx) ex = r.left + rx - px;
    else if (px > r.right - rx) ex = px - (r.right - rx);
    if (py < r.top + ry) ey = r.top + ry - py;
    else if (py > r.bottom - ry) ey = py - (r.bottom - ry);
    if (ex == 0 || ey == 0) return true;
    return (ex * ex) / (rx * rx) + (ey * ey) / (ry * ry) <= 1.0;
}

// verb: 0 frame, 1 paint, 2 erase, 3 invert, 4 fill.
void Player::op_shape(Shape shape, int verb, const Rect& pr) {
    const Rect r{cy(pr.top), cx(pr.left), cy(pr.bottom), cx(pr.right)};
    const Rect inner{r.top + pen_h_, r.left + pen_w_, r.bottom - pen_h_, r.right - pen_w_};
    const Rect area = intersect(r, {0, 0, out_.height, out_.width});
    for (int y = area.top; y < area.bottom; ++y) {
        for (int x = area.left; x < area.right; ++x) {
            if (!inside(shape, r, oval_w_, oval_h_, x, y)) continue;
            switch (verb) {
            case 0:
                if (!inside(shape, inner, oval_w_ - 2 * pen_w_, oval_h_ - 2 * pen_h_, x, y))
                    pat_plot(x, y, pen_pat_, pen_mode_);
                break;
            case 1: pat_plot(x, y, pen_pat_, pen_mode_); break;
            case 2: pat_plot(x, y, bk_pat_, 8); break;
            case 4: pat_plot(x, y, fill_pat_, 8); break;
            default: break;  // invert: needs the destination; skipped
            }
        }
    }
    if (verb == 3) ++out_.skipped;
}

void Player::op_poly(int verb) {
    const std::size_t start = r_.p;
    const std::uint16_t size = r_.u16();
    if (size < 10) {
        r_.ok = false;
        return;
    }
    r_.rect();  // bounding box
    std::vector<std::pair<int, int>> pts;
    for (std::size_t n = (size - 10u) / 4; n-- > 0 && r_.ok;) {
        const int v = r_.s16(), h = r_.s16();
        pts.push_back({h, v});
    }
    r_.p = start + size;
    if (!r_.need(0) || pts.empty()) return;
    if (verb == 3 || verb > 4) {  // invert and the reserved verbs
        ++out_.skipped;
        return;
    }
    if (verb == 0) {
        for (std::size_t i = 1; i < pts.size(); ++i)
            op_line(pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second);
        return;
    }
    // Paint/fill/erase: even-odd scanline fill.
    const Pattern& pat = verb == 2 ? bk_pat_ : verb == 4 ? fill_pat_ : pen_pat_;
    const int mode = verb == 1 ? pen_mode_ : 8;
    for (int y = 0; y < out_.height; ++y) {
        const double sy = y + frame_.top + 0.5;  // the scan line's centre in picture space
        std::vector<double> xs;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto [x0, y0] = pts[i];
            const auto [x1, y1] = pts[(i + 1) % pts.size()];
            if ((y0 <= sy) != (y1 <= sy) && y0 != y1)
                xs.push_back(x0 + (sy - y0) * (x1 - x0) / static_cast<double>(y1 - y0));
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            const int a = cx(static_cast<int>(xs[i] + 0.5)), b = cx(static_cast<int>(xs[i + 1] + 0.5));
            for (int x = std::max(a, 0); x < std::min(b, out_.width); ++x) pat_plot(x, y, pat, mode);
        }
    }
}

// Reads a region. Its bounds come back in picture coordinates; with `mask`, the region is also
// rasterized into it (mask_w x mask_h, offset by -dx, -dy).
bool Player::read_region(Rect& bounds, std::vector<std::uint8_t>* mask, int mask_w, int mask_h, int dx, int dy) {
    const std::size_t start = r_.p;
    const std::uint16_t size = r_.u16();
    bounds = r_.rect();
    if (!r_.ok || size < 10) return false;
    const std::size_t end = start + size;
    if (mask) {
        mask->assign(static_cast<std::size_t>(mask_w) * mask_h, 0);
        if (size == 10) {
            for (int y = std::max(bounds.top - dy, 0); y < std::min(bounds.bottom - dy, mask_h); ++y)
                for (int x = std::max(bounds.left - dx, 0); x < std::min(bounds.right - dx, mask_w); ++x)
                    (*mask)[static_cast<std::size_t>(y) * mask_w + x] = 1;
        } else {
            // Inversion lines: each listed row toggles x spans from then on; rows repeat the last state.
            std::vector<std::uint8_t> row(static_cast<std::size_t>(mask_w), 0);
            int y = 0;
            const auto emit_until = [&](int y_end) {
                for (; y < std::min(y_end, mask_h); ++y)
                    if (y >= 0) std::copy(row.begin(), row.end(), mask->begin() + static_cast<std::ptrdiff_t>(y) * mask_w);
            };
            y = std::max(bounds.top - dy, 0);
            while (r_.ok && r_.p + 2 <= end) {
                const int line = r_.s16();
                if (line == 0x7FFF) break;
                emit_until(line - dy);
                y = std::max(y, line - dy);
                while (r_.ok) {
                    const int x0 = r_.s16();
                    if (x0 == 0x7FFF) break;
                    const int x1 = r_.s16();
                    if (x1 == 0x7FFF) break;
                    for (int x = std::max(x0 - dx, 0); x < std::min(x1 - dx, mask_w); ++x)
                        row[static_cast<std::size_t>(x)] ^= 1;
                }
            }
        }
    }
    r_.p = end;
    return r_.need(0);
}

void Player::op_region(int verb) {
    std::vector<std::uint8_t> mask;
    Rect bounds;
    if (!read_region(bounds, &mask, out_.width, out_.height, frame_.left, frame_.top)) return;
    const Pattern& pat = verb == 2 ? bk_pat_ : verb == 4 ? fill_pat_ : pen_pat_;
    if (verb == 0 || verb >= 3) {  // framing and inverting regions aren't needed; skipped
        ++out_.skipped;
        return;
    }
    for (int y = 0; y < out_.height; ++y)
        for (int x = 0; x < out_.width; ++x)
            if (mask[static_cast<std::size_t>(y) * out_.width + x]) pat_plot(x, y, pat, verb == 1 ? pen_mode_ : 8);
}

void Player::text(int dh, int dv, bool absolute) {
    if (absolute) {
        text_y_ = dv;
        text_x_ = dh;
    } else {
        text_x_ += dh;
        text_y_ += dv;
    }
    const int n = r_.u8();
    const auto s = r_.bytes(static_cast<std::size_t>(n));
    if (!r_.ok) return;
    PictText t;
    t.x = cx(text_x_);
    t.y = cy(text_y_);
    t.font = font_;
    t.size = size_;
    t.face = face_;
    t.text.assign(s.begin(), s.end());
    out_.texts.push_back(std::move(t));
}

bool Player::read_rows(int rows, int row_bytes, std::size_t unpacked_row, bool packed, int unit,
                       std::vector<std::uint8_t>& buf) {
    buf.assign(unpacked_row * static_cast<std::size_t>(rows), 0);
    for (int y = 0; y < rows; ++y) {
        std::uint8_t* row = buf.data() + unpacked_row * static_cast<std::size_t>(y);
        if (!packed || row_bytes < 8) {
            const auto s = r_.bytes(unpacked_row);
            if (!r_.ok) return false;
            std::copy(s.begin(), s.end(), row);
            continue;
        }
        const std::size_t count = row_bytes > 250 ? r_.u16() : r_.u8();
        const auto s = r_.bytes(count);
        if (!r_.ok) return false;
        if (unpack_bits(s, row, unpacked_row, unit) < 0) ++out_.skipped;  // short row: rest stays 0
    }
    return true;
}

bool Player::op_bits(std::uint16_t op, std::string& error) {
    const bool direct = op == 0x9A || op == 0x9B;
    const bool packed = op != 0x90 && op != 0x91;
    const bool region = op == 0x91 || op == 0x99 || op == 0x9B;
    if (direct) r_.u32();  // baseAddr (0xFF)
    const std::uint16_t rb_word = r_.u16();
    const bool pixmap = (rb_word & 0x8000) != 0 || direct;
    const int row_bytes = rb_word & 0x3FFF;
    const Rect bounds = r_.rect();
    int pixel_size = 1, pack_type = 0, cmp_count = 1;
    std::vector<std::uint32_t> clut;  // indexed colours; empty for bitmaps (fg/bk) and direct pixels
    if (pixmap) {
        r_.u16();               // pmVersion
        pack_type = r_.u16();
        r_.skip(12);            // packSize, hRes, vRes
        r_.u16();               // pixelType
        pixel_size = r_.u16();
        cmp_count = r_.u16();
        r_.skip(14);            // cmpSize, planeBytes, pmTable, pmReserved
        if (!direct) {
            r_.u32();  // ctSeed
            const std::uint16_t flags = r_.u16();
            const int n = r_.u16() + 1;
            if (!r_.ok || n > 256) {
                error = "bad colour table";
                return false;
            }
            clut.assign(256, 0);
            for (int i = 0; i < n; ++i) {
                const int value = r_.u16();
                const std::uint16_t cr = r_.u16(), cg = r_.u16(), cb = r_.u16();
                const int index = (flags & 0x8000) ? i : (value & 0xFF);
                clut[static_cast<std::size_t>(index)] = rgb48(cr, cg, cb);
            }
        }
    }
    const Rect src = r_.rect();
    const Rect dst = r_.rect();
    const int mode = r_.u16();
    std::vector<std::uint8_t> rgn_mask;
    if (region) {
        Rect rb;
        if (!read_region(rb, &rgn_mask, out_.width, out_.height, frame_.left, frame_.top)) {
            error = "bad mask region";
            return false;
        }
    }
    if (!r_.ok) {
        error = "truncated bitmap header";
        return false;
    }
    const int w = bounds.width(), h = bounds.height();
    if (w <= 0 || h <= 0 || w > kMaxSide || h > kMaxSide ||
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h) > kMaxArea) {
        error = "bad bitmap bounds";
        return false;
    }
    const bool indexed = !direct;
    if (indexed && pixel_size != 1 && pixel_size != 2 && pixel_size != 4 && pixel_size != 8) {
        error = "unsupported pixel size " + std::to_string(pixel_size);
        return false;
    }
    if (direct && pixel_size != 16 && pixel_size != 32) {
        error = "unsupported direct pixel size " + std::to_string(pixel_size);
        return false;
    }

    // Read the pixel data and expand it to 0xRRGGBB, plus whether each pixel counts as "set" for the
    // boolean modes (bitmaps: a 1 bit; pixmaps: not white).
    std::vector<std::uint8_t> buf;
    std::vector<std::uint32_t> rgb(static_cast<std::size_t>(w) * h);
    std::vector<std::uint8_t> set(rgb.size());
    if (indexed) {
        if (row_bytes * 8 < w * pixel_size) {
            error = "rowBytes too small";
            return false;
        }
        if (!read_rows(h, row_bytes, static_cast<std::size_t>(row_bytes), packed, 1, buf)) {
            error = "truncated pixel data";
            return false;
        }
        const int mask = (1 << pixel_size) - 1;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* row = buf.data() + static_cast<std::size_t>(row_bytes) * y;
            for (int x = 0; x < w; ++x) {
                const int bit = x * pixel_size;
                const int v = (row[bit >> 3] >> (8 - pixel_size - (bit & 7))) & mask;
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (clut.empty()) {
                    rgb[i] = v ? fg_ : bk_;
                    set[i] = static_cast<std::uint8_t>(v);
                } else {
                    rgb[i] = clut[static_cast<std::size_t>(v)];
                    set[i] = rgb[i] != 0xFFFFFF;
                }
            }
        }
    } else if (pixel_size == 16) {
        if (row_bytes < w * 2) {
            error = "rowBytes too small";
            return false;
        }
        const bool rle = pack_type == 0 || pack_type == 3;
        if (!read_rows(h, row_bytes, static_cast<std::size_t>(row_bytes), rle, 2, buf)) {
            error = "truncated pixel data";
            return false;
        }
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* row = buf.data() + static_cast<std::size_t>(row_bytes) * y;
            for (int x = 0; x < w; ++x) {
                const int v = row[2 * x] << 8 | row[2 * x + 1];
                const auto c5 = [](int c) { return static_cast<std::uint32_t>(c << 3 | c >> 2); };
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                rgb[i] = c5((v >> 10) & 31) << 16 | c5((v >> 5) & 31) << 8 | c5(v & 31);
                set[i] = rgb[i] != 0xFFFFFF;
            }
        }
    } else {
        const auto ww = static_cast<std::size_t>(w);
        if (pack_type == 0 || pack_type == 4) {  // component planes per row: [alpha] red green blue
            const int planes = cmp_count == 4 ? 4 : 3;
            if (!read_rows(h, row_bytes, ww * planes, true, 1, buf)) {
                error = "truncated pixel data";
                return false;
            }
            const std::size_t first = static_cast<std::size_t>(planes - 3) * ww;
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* row = buf.data() + ww * planes * y + first;
                for (std::size_t x = 0; x < ww; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * ww + x;
                    rgb[i] = static_cast<std::uint32_t>(row[x]) << 16 | row[ww + x] << 8 | row[2 * ww + x];
                    set[i] = rgb[i] != 0xFFFFFF;
                }
            }
        } else {
            const std::size_t bpp = pack_type == 2 ? 3 : 4;
            if (static_cast<std::size_t>(row_bytes) < ww * bpp && pack_type != 2) {
                error = "rowBytes too small";
                return false;
            }
            const std::size_t row_len = pack_type == 2 ? ww * 3 : static_cast<std::size_t>(row_bytes);
            if (!read_rows(h, row_bytes, row_len, false, 1, buf)) {
                error = "truncated pixel data";
                return false;
            }
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* row = buf.data() + row_len * y + (bpp - 3);
                for (std::size_t x = 0; x < ww; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * ww + x;
                    rgb[i] = static_cast<std::uint32_t>(row[x * bpp]) << 16 | row[x * bpp + 1] << 8 | row[x * bpp + 2];
                    set[i] = rgb[i] != 0xFFFFFF;
                }
            }
        }
    }
    // v2 pixel data ends word-aligned.
    if (v2_ && (r_.p & 1)) r_.p += 1;

    // Copy srcRect (in bounds coordinates) to dstRect, scaling nearest-neighbour, through the clip.
    if (src.empty() || dst.empty()) return true;
    const int base = mode & 0x3F;
    if (base != 0 && base != 1 && base != 3 && base != 4 && base != 36) ++out_.skipped;
    const Rect d{cy(dst.top), cx(dst.left), cy(dst.bottom), cx(dst.right)};
    const Rect area = intersect(d, {0, 0, out_.height, out_.width});
    for (int y = area.top; y < area.bottom; ++y) {
        const int sy = src.top + static_cast<int>(static_cast<long long>(y - d.top) * src.height() / d.height()) - bounds.top;
        if (sy < 0 || sy >= h) continue;
        for (int x = area.left; x < area.right; ++x) {
            if (!rgn_mask.empty() && !rgn_mask[static_cast<std::size_t>(y) * out_.width + x]) continue;
            const int sx = src.left + static_cast<int>(static_cast<long long>(x - d.left) * src.width() / d.width()) - bounds.left;
            if (sx < 0 || sx >= w) continue;
            const std::size_t i = static_cast<std::size_t>(sy) * w + sx;
            switch (base) {
            case 1:  // srcOr: only the set pixels
                if (set[i]) plot(x, y, clut.empty() && !direct ? fg_ : rgb[i]);
                break;
            case 3:  // srcBic: set pixels become the background colour
                if (set[i]) plot(x, y, bk_);
                break;
            case 4:  // notSrcCopy (bitmaps)
                plot(x, y, set[i] ? bk_ : fg_);
                break;
            case 36:  // transparent: everything but the background colour
                if (rgb[i] != bk_) plot(x, y, rgb[i]);
                break;
            default: plot(x, y, rgb[i]); break;
            }
        }
    }
    return true;
}

// Skips a PixPat (BkPixPat / PnPixPat / FillPixPat); its colours aren't used, the 1-bit fallback is.
bool Player::skip_pixpat(std::string& error) {
    const std::uint16_t type = r_.u16();
    Pattern pat{};
    for (auto& b : pat) b = r_.u8();
    if (type == 2) {  // RGB pattern
        r_.skip(6);
    } else if (type == 1) {  // pixmap pattern
        const int row_bytes = r_.u16() & 0x3FFF;
        const Rect bounds = r_.rect();
        r_.skip(2);
        const int pack_type = r_.u16();
        r_.skip(12 + 2);
        const int pixel_size = r_.u16();
        r_.skip(2 + 2 + 12);
        r_.u32();
        r_.u16();
        const int n = r_.u16() + 1;
        r_.skip(static_cast<std::size_t>(n) * 8);
        const int h = bounds.height();
        if (!r_.ok || h < 0 || h > kMaxSide || n > 256 || pixel_size > 32) {
            error = "bad pixel pattern";
            return false;
        }
        std::vector<std::uint8_t> buf;
        if (!read_rows(h, row_bytes, static_cast<std::size_t>(row_bytes), pack_type != 1, 1, buf)) {
            error = "truncated pixel pattern";
            return false;
        }
    }
    return r_.ok;
}

bool Player::run(std::string& error) {
    // A PICT file starts with a 512-byte header that the resource form doesn't have.
    const auto looks_like_pict = [](std::span<const std::uint8_t> d) {
        return d.size() >= 12 && ((d[10] == 0x11 && d[11] == 0x01) ||
                                  (d.size() >= 14 && d[10] == 0x00 && d[11] == 0x11 && d[12] == 0x02 && d[13] == 0xFF));
    };
    if (!looks_like_pict(r_.d) && r_.d.size() > 512 && looks_like_pict(r_.d.subspan(512))) r_.d = r_.d.subspan(512);
    if (!looks_like_pict(r_.d)) {
        error = "not a PICT (no version opcode)";
        return false;
    }
    r_.u16();  // picSize (low word only; unreliable for large pictures)
    frame_ = r_.rect();
    v2_ = r_.d[10] == 0x00;
    out_.version = v2_ ? 2 : 1;
    out_.frame_left = frame_.left;
    out_.frame_top = frame_.top;
    out_.width = frame_.width();
    out_.height = frame_.height();
    if (out_.width <= 0 || out_.height <= 0 || out_.width > kMaxSide || out_.height > kMaxSide ||
        static_cast<std::size_t>(out_.width) * static_cast<std::size_t>(out_.height) > kMaxArea) {
        error = "bad picture frame";
        return false;
    }
    out_.pixels.assign(static_cast<std::size_t>(out_.width) * out_.height, 0);
    clip_ = {0, 0, out_.height, out_.width};

    for (int ops = 0; ops < 1'000'000; ++ops) {
        if (v2_ && (r_.p & 1)) r_.p += 1;  // v2 opcodes are word-aligned
        const std::uint16_t op = v2_ ? r_.u16() : r_.u8();
        if (!r_.ok) {
            error = "picture ends without OpEndPic";
            return false;
        }
        if (op == 0x00FF) return true;
        if (op == 0x0011) {  // version: v1 0x01; v2 0x02FF
            if (v2_) r_.u16();
            else r_.u8();
            continue;
        }
        if (op == 0x0C00) {  // v2 header
            r_.skip(24);
            continue;
        }
        if (op >= 0x0100) {  // reserved / QuickTime: lengths from the opcode itself
            if (op < 0x8000) r_.skip(static_cast<std::size_t>(op >> 8) * 2);
            else if (op >= 0x8100) r_.skip(r_.u32());
            if (!r_.ok) {
                error = "truncated opcode data";
                return false;
            }
            continue;
        }
        switch (op) {
        case 0x00: case 0x17: case 0x18: case 0x19: case 0x1C: case 0x1E: break;
        case 0x01: {  // clip region
            Rect b;
            std::vector<std::uint8_t> mask;
            const std::size_t at = r_.p;
            const std::uint16_t size = r_.u16();
            r_.p = at;
            if (!read_region(b, size > 10 ? &mask : nullptr, out_.width, out_.height, frame_.left, frame_.top)) break;
            clip_ = intersect({cy(b.top), cx(b.left), cy(b.bottom), cx(b.right)}, {0, 0, out_.height, out_.width});
            clip_mask_ = std::move(mask);
            break;
        }
        case 0x02: for (auto& b : bk_pat_) b = r_.u8(); break;
        case 0x03: font_ = r_.u16(); break;
        case 0x04: face_ = r_.u8(); break;
        case 0x05: r_.u16(); break;  // TxMode
        case 0x06: r_.skip(4); break;
        case 0x07: pen_h_ = r_.s16(); pen_w_ = r_.s16(); break;
        case 0x08: pen_mode_ = r_.u16(); break;
        case 0x09: for (auto& b : pen_pat_) b = r_.u8(); break;
        case 0x0A: for (auto& b : fill_pat_) b = r_.u8(); break;
        case 0x0B: oval_h_ = r_.s16(); oval_w_ = r_.s16(); break;
        case 0x0C: {  // Origin: later coordinates are in a shifted system
            const int dv = r_.s16(), dh = r_.s16();
            frame_.left += dh;
            frame_.top += dv;
            break;
        }
        case 0x0D: size_ = r_.u16(); break;
        case 0x0E: fg_ = old_color(r_.u32()); break;
        case 0x0F: bk_ = old_color(r_.u32()); break;
        case 0x10: r_.skip(8); break;  // TxRatio
        case 0x12: case 0x13: case 0x14:
            if (!skip_pixpat(error)) return false;
            break;
        case 0x15: case 0x16: r_.u16(); break;
        case 0x1A: { const auto cr = r_.u16(), cg = r_.u16(), cb = r_.u16(); fg_ = rgb48(cr, cg, cb); break; }
        case 0x1B: { const auto cr = r_.u16(), cg = r_.u16(), cb = r_.u16(); bk_ = rgb48(cr, cg, cb); break; }
        case 0x1D: case 0x1F: r_.skip(6); break;
        case 0x20: {
            const int v0 = r_.s16(), h0 = r_.s16(), v1 = r_.s16(), h1 = r_.s16();
            op_line(h0, v0, h1, v1);
            pen_x_ = h1;
            pen_y_ = v1;
            break;
        }
        case 0x21: {
            const int v1 = r_.s16(), h1 = r_.s16();
            op_line(pen_x_, pen_y_, h1, v1);
            pen_x_ = h1;
            pen_y_ = v1;
            break;
        }
        case 0x22: {
            const int v0 = r_.s16(), h0 = r_.s16();
            const int dh = static_cast<std::int8_t>(r_.u8()), dv = static_cast<std::int8_t>(r_.u8());
            op_line(h0, v0, h0 + dh, v0 + dv);
            pen_x_ = h0 + dh;
            pen_y_ = v0 + dv;
            break;
        }
        case 0x23: {
            const int dh = static_cast<std::int8_t>(r_.u8()), dv = static_cast<std::int8_t>(r_.u8());
            op_line(pen_x_, pen_y_, pen_x_ + dh, pen_y_ + dv);
            pen_x_ += dh;
            pen_y_ += dv;
            break;
        }
        case 0x28: {
            const int v = r_.s16(), h = r_.s16();
            text(h, v, true);
            break;
        }
        case 0x29: text(r_.u8(), 0, false); break;
        case 0x2A: text(0, r_.u8(), false); break;
        case 0x2B: {
            const int dh = r_.u8(), dv = r_.u8();
            text(dh, dv, false);
            break;
        }
        case 0xA0: r_.u16(); break;
        case 0xA1: {
            r_.u16();
            r_.skip(r_.u16());
            break;
        }
        case 0x90: case 0x91: case 0x98: case 0x99: case 0x9A: case 0x9B:
            if (!op_bits(op, error)) return false;
            break;
        default:
            if (op >= 0x30 && op < 0x60) {  // rect, rrect, oval: verbs frame/paint/erase/invert/fill
                const int kind = (op - 0x30) >> 4;
                const int verb = op & 7;
                if (verb > 4) {  // reserved verbs: a rect, or nothing for the "same" forms
                    if ((op & 8) == 0) r_.skip(8);
                    break;
                }
                Rect rc = last_rect_[kind];
                if ((op & 8) == 0) rc = last_rect_[kind] = r_.rect();
                op_shape(kind == 0 ? Shape::Rect : kind == 1 ? Shape::RRect : Shape::Oval, verb, rc);
            } else if (op >= 0x60 && op < 0x70) {  // arcs: not drawn
                if ((op & 8) == 0) last_rect_[3] = r_.rect();
                r_.skip(4);
                ++out_.skipped;
            } else if (op >= 0x70 && op < 0x78) {
                op_poly(op & 7);
            } else if (op >= 0x80 && op < 0x88) {
                op_region(op & 7);
            } else if ((op >= 0x78 && op < 0x80) || (op >= 0x88 && op < 0x90) || (op >= 0xB0 && op < 0xD0)) {
                ++out_.skipped;  // "same poly/region" and reserved no-data opcodes
            } else if ((op >= 0x24 && op < 0x28) || (op >= 0x2C && op < 0x30) || (op >= 0x92 && op < 0x98) ||
                       (op >= 0x9C && op < 0xA0) || (op >= 0xA2 && op < 0xB0)) {
                // fontName, lineJustify, glyphState and reserved: a word length, then data.
                const std::uint16_t n = r_.u16();
                if (op == 0x2C && n >= 2) {
                    const std::size_t at = r_.p;
                    font_ = r_.u16();
                    r_.p = at;
                }
                r_.skip(n);
            } else if (op >= 0xD0 && op < 0xFF) {
                r_.skip(r_.u32());
            } else {
                error = "unknown opcode " + std::to_string(op);
                return false;
            }
            break;
        }
        if (!r_.ok) {
            error = "truncated data for opcode " + std::to_string(op);
            return false;
        }
    }
    error = "too many opcodes";
    return false;
}

}  // namespace

bool decode_pict(std::span<const std::uint8_t> data, Pict& out, std::string* error) {
    out = Pict{};
    std::string err;
    Player player(data, out);
    if (player.run(err)) return true;
    if (error) *error = err;
    return false;
}

}  // namespace vette::assets
