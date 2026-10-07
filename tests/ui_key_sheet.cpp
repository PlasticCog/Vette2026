// The key sheet (Ctrl+H): the panel fits common windows, everything is drawn inside it, and the rest of
// the canvas is left see-through.

#include <algorithm>
#include <cstdint>

#include "test.h"
#include "ui/canvas.h"
#include "ui/key_sheet.h"

using vette::ui::Canvas;
using vette::ui::draw_key_sheet;

TEST(key_sheet_fits_and_stays_in_its_panel) {
    const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960}, {1920, 1080}, {3840, 2160}};
    for (const auto& size : sizes) {
        for (const bool paused : {true, false}) {
            Canvas c;
            draw_key_sheet(c, size[0], size[1], paused);
            CHECK(c.width * c.scale <= size[0] && c.height * c.scale <= size[1]);
            // The drawn pixels' bounds: the panel's border, whole, with a margin to the canvas's edge.
            int x0 = c.width, y0 = c.height, x1 = -1, y1 = -1;
            for (int y = 0; y < c.height; ++y) {
                for (int x = 0; x < c.width; ++x) {
                    if (c.pixels[static_cast<std::size_t>(y * c.width + x)] != c.background) {
                        x0 = std::min(x0, x);
                        y0 = std::min(y0, y);
                        x1 = std::max(x1, x);
                        y1 = std::max(y1, y);
                    }
                }
            }
            CHECK(x0 > 0 && y0 > 0 && x1 < c.width - 1 && y1 < c.height - 1);
            // Only the border on the bounds' edges: no text ran over them.
            const std::uint32_t border = c.pixels[static_cast<std::size_t>(y0 * c.width + x0)];
            bool edges = true;
            for (int x = x0; x <= x1; ++x)
                edges = edges && c.pixels[static_cast<std::size_t>(y0 * c.width + x)] == border &&
                        c.pixels[static_cast<std::size_t>(y1 * c.width + x)] == border;
            for (int y = y0; y <= y1; ++y)
                edges = edges && c.pixels[static_cast<std::size_t>(y * c.width + x0)] == border &&
                        c.pixels[static_cast<std::size_t>(y * c.width + x1)] == border;
            CHECK(edges);
        }
    }
}
