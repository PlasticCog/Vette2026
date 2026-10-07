#pragma once
// A top-down picture of the city, north up and east right: each cell's objects as the original's own
// polygons and lines seen from straight above (ground layer first, then the sortables by height), over
// the big tiles' ground colour, with the compound structures (the bridges' towers) where their cells
// still list them. For vette_world's map and the map editor (ui/map_editor.h), which redraws the cells it
// changes.

#include <cstdint>
#include <vector>

#include "enhanced/world.h"

namespace vette::enhanced {

class TopDown {
public:
    // The map of `world`, `px_per_cell` pixels per cell. `world` must outlive this.
    TopDown(const World& world, int px_per_cell);

    int width() const { return width_; }
    int height() const { return height_; }
    int px_per_cell() const { return px_; }
    const std::vector<uint32_t>& pixels() const { return img_; }  // 0xRRGGBB, row 0 at the top (north)

    // What's in a cell, and a big tile's ground colour (EGA 0..15), for the next redraw.
    void set_cell(int cx, int cy, Cell cell);
    const Cell& cell(int cx, int cy) const { return cells_[static_cast<size_t>(cx * cells_y_ + cy)]; }
    void set_ground(int big_tile, uint8_t colour);

    void render();  // the whole map
    // Cells [cx0, cx1) x [cy0, cy1), with whatever their neighbours' objects reach into them. Returns the
    // pixel rectangle redrawn (x, y, w, h).
    void render_cells(int cx0, int cy0, int cx1, int cy1, int& x, int& y, int& w, int& h);
    bool grid = true;  // big-tile boundaries in white

    // A cell type alone (elevation 0) on `background`, px x px.
    static std::vector<uint32_t> thumbnail(const World& world, int type, int px, uint32_t background);

private:
    TopDown(const World& world, int px_per_cell, int cells_x, int cells_y);  // empty cells, no ground
    struct P {
        double u, v, z;
    };
    P to_px(double x, double y, double z) const { return {y * scale_, height_ - 1 - x * scale_, z}; }
    void draw_window(int cx0, int cy0, int cx1, int cy1);
    void draw(const Variant& v, Vec3i pos);
    void put(int u, int v, uint32_t c) {
        if (u >= clip_x0_ && v >= clip_y0_ && u < clip_x1_ && v < clip_y1_)
            img_[static_cast<size_t>(v * width_ + u)] = c;
    }
    void seg(const P& a, const P& b, uint8_t colour);
    void fill(const std::vector<P>& poly, Colour c);
    bool compound_listed(const CompoundInstance& ci) const;

    const World& world_;
    int px_ = 1;
    double scale_ = 1;
    int cells_x_ = 0, cells_y_ = 0;
    int width_ = 0, height_ = 0;
    std::vector<Cell> cells_;
    std::vector<uint8_t> ground_;  // per big tile
    std::vector<uint32_t> img_;
    int clip_x0_ = 0, clip_y0_ = 0, clip_x1_ = 0, clip_y1_ = 0;
};

}  // namespace vette::enhanced
