#include "enhanced/scene_geometry.h"

#include <cmath>

namespace vette::enhanced::geometry {

namespace {

double cross(double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

// Whether p is inside or on triangle abc (of orientation `sign`), excluding its corners.
bool inside(double px, double py, double ax, double ay, double bx, double by, double cx, double cy, double sign) {
    const double d1 = cross(ax, ay, bx, by, px, py) * sign;
    const double d2 = cross(bx, by, cx, cy, px, py) * sign;
    const double d3 = cross(cx, cy, ax, ay, px, py) * sign;
    return d1 >= 0 && d2 >= 0 && d3 >= 0;
}

} // namespace

bool ear_clip(const float* x, const float* y, int n, std::vector<uint16_t>& out) {
    if (n < 3) {
        return true;
    }
    double area = 0;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        area += double(x[i]) * y[j] - double(x[j]) * y[i];
    }
    const double sign = area >= 0 ? 1.0 : -1.0;
    int idx[256];
    int m = n < 256 ? n : 256;
    for (int i = 0; i < m; ++i) {
        idx[i] = i;
    }
    bool clean = true;
    int guard = 0;
    while (m > 3 && guard++ < 4096) {
        bool found = false;
        for (int k = 0; k < m && !found; ++k) {
            const int a = idx[(k + m - 1) % m], b = idx[k], c = idx[(k + 1) % m];
            const double turn = cross(x[a], y[a], x[b], y[b], x[c], y[c]) * sign;
            if (turn <= 0) {
                if (turn == 0 && (x[a] == x[b] && y[a] == y[b])) {
                    // A repeated point: drop it.
                    for (int q = k; q + 1 < m; ++q) idx[q] = idx[q + 1];
                    --m;
                    found = true;
                }
                continue;
            }
            bool blocked = false;
            for (int q = 0; q < m && !blocked; ++q) {
                const int p = idx[q];
                if (p == a || p == b || p == c) continue;
                if ((x[p] == x[a] && y[p] == y[a]) || (x[p] == x[b] && y[p] == y[b]) || (x[p] == x[c] && y[p] == y[c])) {
                    continue;  // a bridge point coinciding with a corner
                }
                blocked = inside(x[p], y[p], x[a], y[a], x[b], y[b], x[c], y[c], sign);
            }
            if (blocked) continue;
            out.push_back(static_cast<uint16_t>(a));
            out.push_back(static_cast<uint16_t>(b));
            out.push_back(static_cast<uint16_t>(c));
            for (int q = k; q + 1 < m; ++q) idx[q] = idx[q + 1];
            --m;
            found = true;
        }
        if (!found) {
            // Degenerate (collinear or self-touching) remainder: fan it.
            clean = false;
            for (int k = 1; k + 1 < m; ++k) {
                out.push_back(static_cast<uint16_t>(idx[0]));
                out.push_back(static_cast<uint16_t>(idx[k]));
                out.push_back(static_cast<uint16_t>(idx[k + 1]));
            }
            return false;
        }
    }
    if (m == 3) {
        out.push_back(static_cast<uint16_t>(idx[0]));
        out.push_back(static_cast<uint16_t>(idx[1]));
        out.push_back(static_cast<uint16_t>(idx[2]));
    }
    return clean;
}

bool triangulate(const P3* p, int n, std::vector<uint16_t>& out) {
    if (n < 3) {
        return true;
    }
    // Newell normal.
    double nx = 0, ny = 0, nz = 0;
    for (int i = 0; i < n; ++i) {
        const P3& a = p[i];
        const P3& b = p[(i + 1) % n];
        nx += (double(a.y) - b.y) * (double(a.z) + b.z);
        ny += (double(a.z) - b.z) * (double(a.x) + b.x);
        nz += (double(a.x) - b.x) * (double(a.y) + b.y);
    }
    const double ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
    float u[256], v[256];
    const int m = n < 256 ? n : 256;
    for (int i = 0; i < m; ++i) {
        if (az >= ax && az >= ay) {
            u[i] = p[i].x;
            v[i] = p[i].y;
        } else if (ay >= ax) {
            u[i] = p[i].z;
            v[i] = p[i].x;
        } else {
            u[i] = p[i].y;
            v[i] = p[i].z;
        }
    }
    return ear_clip(u, v, m, out);
}

int clip_near(const P3* in, int n, float near, P3* out) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const P3& a = in[i];
        const P3& b = in[(i + 1) % n];
        const bool ia = a.z >= near, ib = b.z >= near;
        if (ia) {
            out[m++] = a;
        }
        if (ia != ib) {
            const float t = (near - a.z) / (b.z - a.z);
            out[m++] = {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), near};
        }
    }
    return m;
}

bool degenerate(const P3* p, int n) {
    if (n < 3) {
        return true;
    }
    double nx = 0, ny = 0, nz = 0;
    for (int i = 0; i < n; ++i) {
        const P3& a = p[i];
        const P3& b = p[(i + 1) % n];
        nx += (double(a.y) - b.y) * (double(a.z) + b.z);
        ny += (double(a.z) - b.z) * (double(a.x) + b.x);
        nz += (double(a.x) - b.x) * (double(a.y) + b.y);
    }
    return nx * nx + ny * ny + nz * nz < 1e-6;
}

}  // namespace vette::enhanced::geometry
