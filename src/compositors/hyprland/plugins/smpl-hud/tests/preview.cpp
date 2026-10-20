// Draws a panel the way the compositor will, as a PPM on stdout, for tuning a
// style by eye. Strips are stretched the way the GPU's bilinear filter does it,
// tiles are copied 1:1 and beads are placed at fractional positions. The
// colours are a fixed amber.
//
//   make preview
//   build/preview [style] [width] [height] [scale] [zoom] [floating] [travelled] > panel.ppm
//
// width and height are the window in physical px; `travelled` is how far the dots have run.

#include "../geometry.hpp"
#include "../gradient.hpp"
#include "../panel.hpp"
#include "../stamp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace smplhud;

namespace {

struct Canvas {
    int                w, h;
    std::vector<float> px; // premultiplied RGBA

    Canvas(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * size_t(h_) * 4, 0.f) {}

    void over(int x, int y, const float c[4]) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        float* d = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
        for (int k = 0; k < 4; ++k)
            d[k] = c[k] + d[k] * (1.f - c[3]);
    }
};

void texel(const Image& img, int x, int y, float out[4]) {
    x = std::clamp(x, 0, img.w - 1);
    y = std::clamp(y, 0, img.h - 1);
    for (int k = 0; k < 4; ++k)
        out[k] = img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4 + size_t(k)] / 255.f;
}

// Draws `img` with its corner at (ox, oy), possibly between pixels, with bilinear filtering.
void drawImage(Canvas& cv, const Image& img, double ox, double oy, float alpha) {
    const int x0 = int(std::floor(ox)) - 1, y0 = int(std::floor(oy)) - 1;
    for (int y = y0; y < y0 + img.h + 3; ++y) {
        for (int x = x0; x < x0 + img.w + 3; ++x) {
            const double u = x + 0.5 - ox - 0.5, v = y + 0.5 - oy - 0.5;
            if (u < -1 || v < -1 || u > img.w || v > img.h)
                continue;
            const int    iu = int(std::floor(u)), iv = int(std::floor(v));
            const double fu = u - iu, fv = v - iv;
            float        c[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const int ix = iu + i, iy = iv + j;
                    if (ix < 0 || iy < 0 || ix >= img.w || iy >= img.h)
                        continue; // outside the sprite is transparent
                    float t[4];
                    texel(img, ix, iy, t);
                    const float wgt = float((i ? fu : 1 - fu) * (j ? fv : 1 - fv));
                    for (int k = 0; k < 4; ++k)
                        c[k] += t[k] * wgt;
                }
            }
            for (float& k : c)
                k *= alpha;
            cv.over(x, y, c);
        }
    }
}

void drawStrip(Canvas& cv, const StripPiece& sp, const Stroke& stroke, const Tone& tone, int ox, int oy) {
    const StripSpec spec = stripSpecOf(sp, stroke, tone, tone);
    const Image     img  = bakeStrip(spec);
    const int       len = sp.vertical ? sp.box.h : sp.box.w, size = spec.size;
    for (int i = 0; i < len; ++i) {
        const float t = float(i + 0.5) / float(len);
        for (int j = 0; j < size; ++j) {
            float a[4], b[4], c[4];
            texel(img, sp.vertical ? j : 0, sp.vertical ? 0 : j, a);
            texel(img, sp.vertical ? j : 1, sp.vertical ? 1 : j, b);
            for (int k = 0; k < 4; ++k)
                c[k] = a[k] + (b[k] - a[k]) * t;
            cv.over(ox + sp.box.x + (sp.vertical ? j : i), oy + sp.box.y + (sp.vertical ? i : j), c);
        }
    }
}

bool insideWindow(double x, double y, int w, int h, double cut) {
    if (x < 0 || y < 0 || x >= w || y >= h)
        return false;
    const double dx = std::min(x, w - x), dy = std::min(y, h - y);
    return !(dx < cut && dy < cut && dx + dy < cut);
}

} // namespace

int main(int argc, char** argv) {
    const char*  name     = argc > 1 ? argv[1] : "panel";
    const int    width    = argc > 2 ? std::atoi(argv[2]) : 900;
    const int    height   = argc > 3 ? std::atoi(argv[3]) : 560;
    const float  scale    = argc > 4 ? float(std::atof(argv[4])) : 1.f;
    const int    zoom     = argc > 5 ? std::max(1, std::atoi(argv[5])) : 1;
    const bool   floating = argc > 6 && std::atoi(argv[6]) != 0;
    const double travel   = argc > 7 ? std::atof(argv[7]) : 0;

    const Style* style = styleFor(name);
    if (!style) {
        std::fprintf(stderr, "unknown style %s\n", name);
        return 1;
    }

    Params p;
    p.width          = width;
    p.height         = height;
    p.scale          = scale;
    p.cut            = int(std::lround(21 * scale));
    p.roundingPower  = 1.f;
    p.floating       = floating;
    p.gap            = 15;
    p.lineWidth      = 2;
    p.chamfer        = 10;
    p.floatGlowRange = 14;

    const Layout layout = buildLayout(*style, p);
    if (!layout.valid) {
        std::fprintf(stderr, "no layout\n");
        return 1;
    }

    const int margin = 12;
    const int left   = int(std::ceil(layout.extents.left * scale)) + margin, top = int(std::ceil(layout.extents.top * scale)) + margin;
    const int right  = int(std::ceil(layout.extents.right * scale)) + margin, bottom = int(std::ceil(layout.extents.bottom * scale)) + margin;
    Canvas    cv(left + width + right, top + height + bottom);

    const float bg[4] = {0.05f, 0.055f, 0.07f, 1.f};
    for (int y = 0; y < cv.h; ++y)
        for (int x = 0; x < cv.w; ++x)
            cv.over(x, y, bg);

    const Rgba  line{1.0f, 0.69f, 0.0f, 1.f}, glow{1.0f, 0.45f, 0.05f, 1.f};
    const Rgba  bright{line.r + (1 - line.r) * 0.55f, line.g + (1 - line.g) * 0.55f, line.b + (1 - line.b) * 0.55f, 1.f};
    const Tone  lineTone = makeTone(line, glow), beadTone = makeTone(bright, glow);

    for (const Stroke& s : layout.panel.strokes) {
        for (const StripPiece& sp : s.strips)
            drawStrip(cv, sp, s, lineTone, left, top);
        for (const TilePiece& t : s.tiles)
            drawImage(cv, bakeTile(tileSpecOf(t, s, lineTone)), left + t.box.x, top + t.box.y, 1.f);
    }
    for (const Bead& b : layout.panel.beads)
        drawImage(cv, bakeBead({lookOf(b), beadTone}), left + b.pos.x - beadSize(lookOf(b)) / 2.0, top + b.pos.y - beadSize(lookOf(b)) / 2.0, 1.f);
    const std::vector<RunnerPose> poses = runnerPoses(layout.panel, travel);
    auto drawBeadAt = [&](const Bead& bead, const RunnerPose& pose, float alpha) {
        if (alpha > 0.f)
            drawImage(cv, bakeBead({lookOf(bead), beadTone}), left + pose.pos.x - beadSize(lookOf(bead)) / 2.0, top + pose.pos.y - beadSize(lookOf(bead)) / 2.0, alpha);
    };
    for (size_t i = 0; i < poses.size(); ++i) {
        drawBeadAt(layout.panel.runners[i].look, poses[i], poses[i].alpha);
        drawBeadAt(layout.panel.flare, poses[i], poses[i].alpha * poses[i].glow);
    }

    // The window, a little see-through like the real ones.
    const float win[4] = {0.09f * 0.85f, 0.10f * 0.85f, 0.12f * 0.85f, 0.85f};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (insideWindow(x + 0.5, y + 0.5, width, height, p.cut))
                cv.over(left + x, top + y, win);

    std::printf("P6\n%d %d\n255\n", cv.w * zoom, cv.h * zoom);
    for (int y = 0; y < cv.h * zoom; ++y) {
        for (int x = 0; x < cv.w * zoom; ++x) {
            const float* c = &cv.px[(size_t(y / zoom) * size_t(cv.w) + size_t(x / zoom)) * 4];
            for (int k = 0; k < 3; ++k)
                std::putchar(int(std::lround(std::clamp(c[k], 0.f, 1.f) * 255)));
        }
    }
    return 0;
}
