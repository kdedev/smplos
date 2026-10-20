// Draws a panel the way the compositor will, as a PPM on stdout, for tuning a
// style by eye. Strips are stretched the way the GPU's bilinear filter does it,
// tiles are copied 1:1, beads and characters are placed at whole or fractional
// positions and the travelling glow is a sprite stretched the same way. The
// colours are a fixed amber, or a fixed green with the palette "matrix".
//
//   make preview
//   build/preview [style] [width] [height] [scale] [zoom] [floating] [travelled] [clock] [palette] [intensity] [seed] [gap] [side] > panel.ppm
//   build/preview glyphs [zoom] [palette] > glyphs.ppm
//
// width and height are the window in physical px; `travelled` is how far the dots and the rain have run
// and `clock` how long the lamps of a glyph style have been running, in seconds;
// `intensity` is how far the window has faded into focus, 0 to 1; `gap` is how far the outline
// stands off the window (`hud_gap`, 15 if not given; the Matrix HUD has 3); `side` is how visible
// the outline is down its left and right sides (`hud_side_opacity`, 0 to 1, 1 if not given). `glyphs`
// draws every character of the matrix style, twelve to a row, in the order of the table.

#include "../geometry.hpp"
#include "../glyph.hpp"
#include "../gradient.hpp"
#include "../panel.hpp"
#include "../stamp.hpp"
#include "../sweep.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
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

// The sprite of the travelling glow, which has one texel for every `texel` px.
void drawSprite(Canvas& cv, const Sprite& sp, int ox, int oy, float alpha) {
    Image img;
    img.w    = sp.width;
    img.h    = sp.height;
    img.rgba = sp.rgba;
    for (int y = sp.box.y; y < sp.box.y + sp.box.h; ++y) {
        for (int x = sp.box.x; x < sp.box.x + sp.box.w; ++x) {
            const double u = (x + 0.5 - sp.box.x) / sp.texel - 0.5, v = (y + 0.5 - sp.box.y) / sp.texel - 0.5;
            const int    iu = int(std::floor(u)), iv = int(std::floor(v));
            const double fu = u - iu, fv = v - iv;
            float        c[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    float t[4];
                    texel(img, iu + i, iv + j, t);
                    const float wgt = float((i ? fu : 1 - fu) * (j ? fv : 1 - fv));
                    for (int k = 0; k < 4; ++k)
                        c[k] += t[k] * wgt;
                }
            }
            for (float& k : c)
                k *= alpha;
            cv.over(ox + x, oy + y, c);
        }
    }
}

struct Palette {
    Rgba line, glow, bright;
    bool ownGlyphs = false; // the characters of a glyph style have a colour of their own, as the theme can give them
    Rgba glyph{}, glyphBright{};
};

Rgba lighter(const Rgba& c, float amount) {
    return {c.r + (1 - c.r) * amount, c.g + (1 - c.g) * amount, c.b + (1 - c.b) * amount, 1.f};
}

Palette paletteOf(const std::string& name, float whiten) {
    Palette p;
    if (name == "matrix") {
        p.line      = {0.0f, 1.0f, 0.25f, 1.f};
        p.glow      = {0.0f, 0.75f, 0.45f, 1.f};
        p.ownGlyphs = true; // as `hud_glyph_color` of matrix-hud
        p.glyph     = {0x3c / 255.f, 1.0f, 0x28 / 255.f, 1.f};
    } else {
        p.line = {1.0f, 0.69f, 0.0f, 1.f};
        p.glow = {1.0f, 0.45f, 0.05f, 1.f};
    }
    p.bright      = lighter(p.line, whiten);
    p.glyphBright = lighter(p.glyph, whiten);
    return p;
}

// The colours of a character and of its glow, and of the same one when it is hot: those of the line, or the
// theme's own colour for characters.
struct GlyphTones {
    Tone line, bright;
};

GlyphTones glyphTonesOf(const Palette& pal) {
    const Rgba& body = pal.ownGlyphs ? pal.glyph : pal.line;
    const Rgba& hot  = pal.ownGlyphs ? pal.glyphBright : pal.bright;
    const Rgba& halo = pal.ownGlyphs ? pal.glyph : pal.glow;
    return {makeTone(body, halo, 7), makeTone(hot, halo, 7)};
}

// What the pass element does for a character: the character in the size it was given and the face of its column,
// in the colours of the line and, as hot as it is, the same one over it in the bright colour.
void drawGlyphs(Canvas& cv, const Panel& panel, const std::vector<GlyphDraw>& draws, const Palette& pal, int ox, int oy) {
    GlyphFace faces[kGlyphColumns][kGlyphSizes];
    GlyphSize sizes[kGlyphColumns][kGlyphSizes];
    for (int column = 0; column < kGlyphColumns; ++column) {
        const GlyphFace& base = panel.face(column);
        for (int i = 0; i < kGlyphSizes; ++i) {
            faces[column][i] = glyphFaceOf(base, i);
            sizes[column][i] = glyphSize(faces[column][i]);
        }
    }

    const GlyphTones tones = glyphTonesOf(pal);
    for (const size_t at : glyphOrder(draws)) {
        const GlyphDraw& g      = draws[at];
        const int        column = std::clamp(g.column, 0, kGlyphColumns - 1);
        const int        which  = std::clamp(g.size, 0, kGlyphSizes - 1);
        const GlyphFace& face   = faces[column][which];
        const double     x = ox + g.x - sizes[column][which].w / 2.0, y = oy + g.y - sizes[column][which].h / 2.0;
        const float      lit = float(g.alpha) / kGlyphAlphaSteps;
        drawImage(cv, bakeGlyph(glyphSpecOf(face, g.glyph, tones.line)), x, y, lit);
        if (g.heat > 0) {
            GlyphSpec hot = glyphSpecOf(face, g.glyph, tones.bright);
            hot.knock     = 0;
            drawImage(cv, bakeGlyph(hot), x, y, lit * float(g.heat) / kGlyphHeatSteps);
        }
    }
}

void writePpm(const Canvas& cv, int zoom) {
    std::printf("P6\n%d %d\n255\n", cv.w * zoom, cv.h * zoom);
    for (int y = 0; y < cv.h * zoom; ++y) {
        for (int x = 0; x < cv.w * zoom; ++x) {
            const float* c = &cv.px[(size_t(y / zoom) * size_t(cv.w) + size_t(x / zoom)) * 4];
            for (int k = 0; k < 3; ++k)
                std::putchar(int(std::lround(std::clamp(c[k], 0.f, 1.f) * 255)));
        }
    }
}

Params paramsFor(int width, int height, float scale, bool floating, double gap = 15, double sideOpacity = 1) {
    Params p;
    p.width          = width;
    p.height         = height;
    p.scale          = scale;
    p.cut            = int(std::lround(21 * scale));
    p.roundingPower  = 1.f;
    p.floating       = floating;
    p.gap            = gap;
    p.lineWidth      = 2;
    p.chamfer        = 10;
    p.floatGlowRange = 14;
    p.sideOpacity    = std::clamp(sideOpacity, 0.0, 1.0);
    return p;
}

// Every character, in the order of the table, at full strength.
int sheet(int zoom, const std::string& palette) {
    const Style* style = styleFor("matrix");
    if (!style)
        return 1;

    const Layout layout = buildLayout(*style, paramsFor(400, 300, 1.f, false));
    if (!layout.valid || !layout.panel.glyphs.on())
        return 1;

    const GlyphFace& face = layout.panel.glyphs;
    const GlyphSize  size = glyphSize(face);
    const int        cols = 12, pad = 6, cw = size.w + pad, ch = size.h + pad;
    const int        rows = (glyphCount() + cols - 1) / cols;
    Canvas           cv(cols * cw + pad, rows * ch + pad);

    const float bg[4] = {0.0f, 0.0f, 0.0f, 1.f};
    for (int y = 0; y < cv.h; ++y)
        for (int x = 0; x < cv.w; ++x)
            cv.over(x, y, bg);

    const Palette pal  = paletteOf(palette, style->brightWhiten);
    const Tone    tone = glyphTonesOf(pal).line;
    for (int i = 0; i < glyphCount(); ++i)
        drawImage(cv, bakeGlyph(glyphSpecOf(face, i, tone)), pad + (i % cols) * cw, pad + (i / cols) * ch, 1.f);

    writePpm(cv, zoom);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* name = argc > 1 ? argv[1] : "panel";
    if (std::string(name) == "glyphs")
        return sheet(argc > 2 ? std::max(1, std::atoi(argv[2])) : 4, argc > 3 ? argv[3] : "matrix");

    const int         width    = argc > 2 ? std::atoi(argv[2]) : 900;
    const int         height   = argc > 3 ? std::atoi(argv[3]) : 560;
    const float       scale    = argc > 4 ? float(std::atof(argv[4])) : 1.f;
    const int         zoom     = argc > 5 ? std::max(1, std::atoi(argv[5])) : 1;
    const bool        floating = argc > 6 && std::atoi(argv[6]) != 0;
    const double      travel   = argc > 7 ? std::atof(argv[7]) : 0;
    const double      clock    = argc > 8 ? std::atof(argv[8]) : 0;
    const std::string palette  = argc > 9 ? argv[9] : "amber";
    const double      level    = argc > 10 ? std::clamp(std::atof(argv[10]), 0.0, 1.0) : 1.0;
    const unsigned    seed     = argc > 11 ? unsigned(std::atoi(argv[11])) : 0;
    const double      gap      = argc > 12 ? std::atof(argv[12]) : 15;
    const double      side     = argc > 13 ? std::atof(argv[13]) : 1;

    const Style* style = styleFor(name);
    if (!style) {
        std::fprintf(stderr, "unknown style %s\n", name);
        return 1;
    }

    const Params p = paramsFor(width, height, scale, floating, gap, side);

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

    const Palette pal       = paletteOf(palette, style->brightWhiten);
    const Rgba&   line      = pal.line;
    const Rgba&   glow      = pal.glow;
    const Tone    lineTone  = makeTone(line, glow), beadTone = makeTone(pal.bright, glow);
    const bool    glyphMode = layout.panel.glyphs.on();
    const double  intensity = sweepIntensity(level);

    for (const Stroke& s : layout.panel.strokes) {
        for (const StripPiece& sp : s.strips)
            drawStrip(cv, sp, s, lineTone, left, top);
        for (const TilePiece& t : s.tiles)
            drawImage(cv, bakeTile(tileSpecOf(t, s, lineTone)), left + t.box.x, top + t.box.y, 1.f);
    }
    if (!glyphMode) {
        for (const Bead& b : layout.panel.beads)
            drawImage(cv, bakeBead({lookOf(b), beadTone}), left + b.pos.x - beadSize(lookOf(b)) / 2.0, top + b.pos.y - beadSize(lookOf(b)) / 2.0, 1.f);
    }

    // The travelling glow, where the theme would have it for this travel: round
    // the outline, or for a lamp style the lamps along the top and the bottom edge.
    if (style->sweepAlpha > 0 && intensity > 0 && !layout.panel.strokes.empty()) {
        const bool lamps = !layout.panel.lamps.empty();
        const Path path  = buildPath(lamps ? layout.panel.track : layout.panel.strokes.front().pts);
        if (path.valid()) {
            const int  texel = sweepTexel(scale);
            const int  l = int(std::ceil(layout.extents.left * scale)), t = int(std::ceil(layout.extents.top * scale));
            const int  r = int(std::ceil(layout.extents.right * scale)), b = int(std::ceil(layout.extents.bottom * scale));
            const IBox limit = {-l, -t, width + l + r, height + t + b};

            const auto drawGlow = [&](double at, double length, double strength) {
                const SweepShape shape = {180 * scale / 2 * length, layout.centre.reachOut, layout.centre.reachIn, style->sweepWhiten, style->sweepAlpha};
                drawSprite(cv, renderSweep(path, at, shape, {line.r, line.g, line.b, 1.f}, texel, limit), left, top, float(intensity * strength));
            };
            if (lamps) {
                for (size_t i = 0; i < layout.panel.lamps.size(); ++i) {
                    const LampPose pose = lampPose(layout.panel, i, travel, clock, seed);
                    drawGlow(pose.at, pose.length, pose.strength);
                }
            } else {
                drawGlow(std::fmod(travel / std::max(double(style->runnerSpeed), 1e-9), path.length), 1.0, 1.0);
            }
        }
    }

    if (glyphMode) {
        drawGlyphs(cv, layout.panel, glyphDraws(layout.panel, travel, intensity, seed), pal, left, top);
    } else {
        const std::vector<RunnerPose> poses = intensity > 0 ? runnerPoses(layout.panel, travel) : std::vector<RunnerPose>{};
        auto drawBeadAt = [&](const Bead& bead, const RunnerPose& pose, float alpha) {
            if (alpha > 0.f)
                drawImage(cv, bakeBead({lookOf(bead), beadTone}), left + pose.pos.x - beadSize(lookOf(bead)) / 2.0, top + pose.pos.y - beadSize(lookOf(bead)) / 2.0, alpha);
        };
        for (size_t i = 0; i < poses.size(); ++i) {
            drawBeadAt(layout.panel.runners[i].look, poses[i], poses[i].alpha * float(intensity));
            drawBeadAt(layout.panel.flare, poses[i], poses[i].alpha * poses[i].glow * float(intensity));
        }
    }

    // The window, a little see-through like the real ones.
    const float win[4] = {0.09f * 0.85f, 0.10f * 0.85f, 0.12f * 0.85f, 0.85f};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (insideWindow(x + 0.5, y + 0.5, width, height, p.cut))
                cv.over(left + x, top + y, win);

    writePpm(cv, zoom);
    return 0;
}
