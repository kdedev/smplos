#include "HudPassElement.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include <drm_fourcc.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "gradient.hpp"
#include "stamp.hpp"

using namespace Render::GL;

namespace {
    // Colour of `gradient` at (x, y), normalised over `area` the way the
    // border shader normalises over the box it paints.
    smplhud::OkLabA sampleAt(const Config::CGradientValueData& gradient, const smplhud::IBox& area, double x, double y) {
        if (gradient.m_colorsOkLabA.empty())
            return {};

        const float nx = (x - area.x) / std::max(area.w, 1);
        const float ny = (y - area.y) / std::max(area.h, 1);
        return smplhud::sampleGradient(gradient.m_colorsOkLabA, gradient.m_angle, std::clamp(nx, 0.F, 1.F), std::clamp(ny, 0.F, 1.F));
    }

    // What `paint` looks like at (x, y), mid-fade or not.
    smplhud::Rgba paintAt(const SHudPaint& paint, const smplhud::IBox& area, double x, double y) {
        auto lab = sampleAt(paint.to, area, x, y);
        if (paint.fading && !paint.from.m_colorsOkLabA.empty())
            lab = smplhud::mixOkLabA(sampleAt(paint.from, area, x, y), lab, paint.lerp);
        return smplhud::toRgba(lab);
    }

    // The glow's colour is kept to 5 bits a channel: a gradient that drifts
    // slowly re-renders the glow only once the change could be seen.
    constexpr int kColourLevels = 31;

    uint8_t quantised(float v) {
        return uint8_t(std::lround(std::clamp(v, 0.F, 1.F) * kColourLevels));
    }

    float dequantised(uint8_t q) {
        return float(q) / kColourLevels;
    }

    // Baked images of the lines, dots and characters of panel styles (see
    // stamp.hpp), kept on the GPU and shared by all windows, so each is baked and
    // uploaded once. The key holds everything the image depends on. A full cache
    // is emptied rather than managed: the next frame bakes what it needs again.
    // The characters are the most numerous: some sixty of them in three sizes
    // and a few tones.
    std::unordered_map<std::string, SP<Render::ITexture>> g_stamps;
    constexpr size_t                                      kMaxStamps = 4096;

    template <class Bake>
    SP<Render::ITexture> stampOf(const std::string& key, Bake&& bake) {
        if (const auto it = g_stamps.find(key); it != g_stamps.end())
            return it->second;

        auto image = bake();
        if (image.empty())
            return nullptr;

        auto texture = g_pHyprRenderer->createTexture(DRM_FORMAT_ABGR8888, image.rgba.data(), uint32_t(image.w) * 4, {double(image.w), double(image.h)});
        if (!texture || !texture->ok())
            return nullptr;

        if (g_stamps.size() >= kMaxStamps)
            g_stamps.clear();
        g_stamps.emplace(key, texture);
        return texture;
    }

    // `box` grown to whole px, for a box that need not sit on the pixel grid.
    CBox outward(const CBox& box) {
        const double x = std::floor(box.x), y = std::floor(box.y);
        return {x, y, std::ceil(box.x + box.width) - x, std::ceil(box.y + box.height) - y};
    }

    // Paints premultiplied textures over exactly the damage they are given:
    // with a clip set the texture shader would paint the clip instead, and
    // with a window set it would dim the texture like the window. Both are set
    // aside while one of these lives, and the clip is applied by hand.
    class CTextureScope {
      public:
        CTextureScope() {
            auto& render   = g_pHyprRenderer->m_renderData;
            m_clip         = render.clipBox;
            m_window       = render.currentWindow;
            render.clipBox = {};
            render.currentWindow.reset();
        }

        ~CTextureScope() {
            auto& render         = g_pHyprRenderer->m_renderData;
            render.currentWindow = m_window;
            render.clipBox       = m_clip;
        }

        CTextureScope(const CTextureScope&)            = delete;
        CTextureScope& operator=(const CTextureScope&) = delete;

        // The part of the frame's damage that `box` covers, inside the clip too.
        CRegion damageIn(const CBox& box) const {
            CRegion damage = g_pHyprRenderer->m_renderData.damage.copy().intersect(box);
            if (m_clip.width != 0 && m_clip.height != 0)
                damage.intersect(m_clip);
            return damage;
        }

        void draw(const SP<Render::ITexture>& texture, const CBox& box, CRegion& damage, float alpha) const {
            CHyprOpenGLImpl::STextureRenderData data;
            data.damage   = &damage;
            data.a        = alpha;
            data.allowDim = false;
            g_pHyprOpenGL->renderTexture(texture, box, data);
        }

      private:
        CBox                                                  m_clip;
        decltype(g_pHyprRenderer->m_renderData.currentWindow) m_window;
    };

    // The colours of a line and of its glow at (x, y).
    smplhud::Tone toneAt(const SHudFrame& frame, smplhud::Paint core, smplhud::Paint halo, double x, double y, int levels = smplhud::kToneLevels) {
        const auto& area = frame.layout->sampleBox;
        return smplhud::makeTone(paintAt(frame.paints[size_t(core)], area, x, y), paintAt(frame.paints[size_t(halo)], area, x, y), levels);
    }

    // A dot of the panel centred on `pos`, window-relative physical px.
    void drawBead(const CTextureScope& scope, const SHudFrame& frame, const smplhud::Bead& bead, const smplhud::Vec2& pos, float alpha) {
        if (alpha < 0.002F)
            return;

        const auto look = smplhud::lookOf(bead);
        const int  size = smplhud::beadSize(look);
        const CBox box  = {frame.window.x + pos.x - size / 2.0, frame.window.y + pos.y - size / 2.0, double(size), double(size)};
        auto       damage = scope.damageIn(outward(box));
        if (damage.empty())
            return;

        const smplhud::BeadSpec spec{look, toneAt(frame, smplhud::Paint::Bright, smplhud::Paint::Glow, pos.x, pos.y)};
        if (const auto texture = stampOf(smplhud::keyOf(spec), [&] { return smplhud::bakeBead(spec); }))
            scope.draw(texture, box, damage, alpha);
    }

    // The lines of a panel and the dots that stay where they are. A strip is
    // stretched over a quad twice as long as the run and only the run's own
    // part, its middle half, is painted: the filter blends from the colour at
    // one end of it to the colour at the other.
    void drawPanelLines(const SHudFrame& frame) {
        const auto&   panel = frame.layout->panel;
        const double  ox = frame.window.x, oy = frame.window.y;
        CTextureScope scope;

        for (const auto& stroke : panel.strokes) {
            for (const auto& strip : stroke.strips) {
                const CBox piece  = {ox + strip.box.x, oy + strip.box.y, double(strip.box.w), double(strip.box.h)};
                auto       damage = scope.damageIn(piece);
                if (damage.empty())
                    continue;

                const auto spec = smplhud::stripSpecOf(strip, stroke, toneAt(frame, stroke.core, stroke.halo, strip.a.x, strip.a.y), toneAt(frame, stroke.core, stroke.halo, strip.b.x, strip.b.y));
                if (const auto texture = stampOf(smplhud::keyOf(spec), [&] { return smplhud::bakeStrip(spec); })) {
                    const auto quad = smplhud::stripQuad(strip);
                    scope.draw(texture, {ox + quad.x, oy + quad.y, quad.w, quad.h}, damage, frame.alpha);
                }
            }

            for (const auto& tile : stroke.tiles) {
                const CBox box    = {ox + tile.box.x, oy + tile.box.y, double(tile.box.w), double(tile.box.h)};
                auto       damage = scope.damageIn(box);
                if (damage.empty())
                    continue;

                const auto spec = smplhud::tileSpecOf(tile, stroke, toneAt(frame, stroke.core, stroke.halo, tile.box.x + tile.box.w * 0.5, tile.box.y + tile.box.h * 0.5));
                if (const auto texture = stampOf(smplhud::keyOf(spec), [&] { return smplhud::bakeTile(spec); }))
                    scope.draw(texture, box, damage, frame.alpha);
            }
        }

        for (const auto& bead : panel.beads)
            drawBead(scope, frame, bead, bead.pos, frame.alpha * bead.alpha);
    }

    // The dots that travel along the lines while the window has focus. One that
    // glows has the flare drawn over it, more and more opaque as the glow rises.
    void drawPanelDots(const SHudFrame& frame) {
        const auto& panel   = frame.layout->panel;
        const auto& runners = panel.runners;
        if (runners.empty() || frame.runners.size() != runners.size())
            return;

        CTextureScope scope;
        for (size_t i = 0; i < runners.size(); ++i) {
            const auto& pose  = frame.runners[i];
            const float alpha = frame.alpha * pose.alpha;
            drawBead(scope, frame, runners[i].look, pose.pos, alpha);
            if (pose.glow > 0.F)
                drawBead(scope, frame, panel.flare, pose.pos, alpha * pose.glow);
        }
    }

    // A character is small and its colour changes slowly along the line, so
    // 8 steps a channel are enough to keep the images that are baked few.
    constexpr int kGlyphToneLevels = 7;

    // The colours of a character at (x, y) and of its glow: those of the line
    // there, or the theme's own colour for characters with the opacity the line
    // has there.
    smplhud::Tone glyphToneAt(const SHudFrame& frame, smplhud::Paint core, double x, double y) {
        const auto& area = frame.layout->sampleBox;
        auto        line = paintAt(frame.paints[size_t(core)], area, x, y);
        auto        glow = paintAt(frame.paints[size_t(smplhud::Paint::Glow)], area, x, y);
        if (const auto& own = frame.glyphColour) {
            const auto& hue = core == smplhud::Paint::Bright ? own->bright : own->line;
            line            = {hue.r, hue.g, hue.b, line.a};
            glow            = {own->line.r, own->line.g, own->line.b, glow.a};
        }
        return smplhud::makeTone(line, glow, kGlyphToneLevels);
    }

    // The characters of a glyph style, each in the colours of the line where it
    // stands (on a dark plate that dims the line behind it, if the style has one),
    // in the size it was given and the face of its column. One that is hot has the
    // same character drawn over it in the bright colour (without a plate), as
    // strongly as it is hot; the glow of the two adds up.
    void drawGlyphs(const SHudFrame& frame) {
        const auto& panel = frame.layout->panel;
        if (!panel.glyphs.on() || frame.glyphs.empty())
            return;

        // The faces of the column next to the line, then of the ones beyond it.
        std::array<std::array<smplhud::GlyphFace, smplhud::kGlyphSizes>, smplhud::kGlyphColumns> faces;
        std::array<std::array<smplhud::GlyphSize, smplhud::kGlyphSizes>, smplhud::kGlyphColumns> sizes;
        for (size_t column = 0; column < faces.size(); ++column) {
            const auto& base = panel.face(int(column));
            for (int i = 0; i < smplhud::kGlyphSizes; ++i) {
                faces[column][size_t(i)] = smplhud::glyphFaceOf(base, i);
                sizes[column][size_t(i)] = smplhud::glyphSize(faces[column][size_t(i)]);
            }
        }

        // The columns further away go first, and those nearer lie over them.
        CTextureScope scope;
        for (const size_t at : smplhud::glyphOrder(frame.glyphs)) {
            const auto& glyph  = frame.glyphs[at];
            const auto  column = size_t(std::clamp(glyph.column, 0, smplhud::kGlyphColumns - 1));
            const auto  which  = size_t(std::clamp(glyph.size, 0, smplhud::kGlyphSizes - 1));
            const auto& face   = faces[column][which];
            const auto& size   = sizes[column][which];
            if (size.w <= 0 || size.h <= 0)
                continue;

            const CBox box    = {frame.window.x + glyph.x - size.w / 2.0, frame.window.y + glyph.y - size.h / 2.0, double(size.w), double(size.h)};
            auto       damage = scope.damageIn(outward(box));
            if (damage.empty())
                continue;

            const auto lay = [&](smplhud::Paint core, float strength, bool plate) {
                if (frame.alpha * strength < 0.002F)
                    return;

                auto spec = smplhud::glyphSpecOf(face, glyph.glyph, glyphToneAt(frame, core, glyph.x, glyph.y));
                if (!plate)
                    spec.knock = 0;
                if (const auto texture = stampOf(smplhud::keyOf(spec), [&] { return smplhud::bakeGlyph(spec); })) {
                    auto region = damage.copy();
                    scope.draw(texture, box, region, frame.alpha * strength);
                }
            };

            const float lit = float(glyph.alpha) / smplhud::kGlyphAlphaSteps;
            lay(smplhud::Paint::Line, lit, true);
            if (glyph.heat > 0)
                lay(smplhud::Paint::Bright, lit * float(glyph.heat) / smplhud::kGlyphHeatSteps, false);
        }
    }

    // The travelling glow: a small texture, redrawn on the CPU only when the
    // glow has moved or changed colour, laid over the line under its centre.
    void drawSweep(const SHudFrame& frame, const SHudSweep& sweep) {
        if (!sweep.path || !sweep.cache || sweep.intensity * frame.alpha < 0.002F)
            return;

        const auto box = smplhud::sweepBounds(*sweep.path, sweep.at, sweep.shape, sweep.texel, sweep.limit);
        if (box.w <= 0 || box.h <= 0)
            return;

        // The glow is premultiplied RGBA and goes only where the damage says
        const CTextureScope scope;
        const CBox          target = {box.x + frame.window.x, box.y + frame.window.y, double(box.w), double(box.h)};
        CRegion             damage = scope.damageIn(target);
        if (damage.empty())
            return;

        const auto centre = smplhud::pointAt(*sweep.path, sweep.at).pos;
        auto       rgba   = paintAt(frame.paints[size_t(smplhud::Paint::Line)], frame.layout->sampleBox, centre.x, centre.y);
        // A theme that has a colour of its own for its characters has its glows in that colour, not
        // in the line's, which may start out nearly white.
        if (const auto& own = frame.glyphColour)
            rgba = {own->line.r, own->line.g, own->line.b, rgba.a};
        const std::array<uint8_t, 4> colour = {quantised(rgba.r), quantised(rgba.g), quantised(rgba.b), quantised(rgba.a)};
        if (colour[3] == 0)
            return;

        auto& cache = *sweep.cache;
        if (!cache.texture || cache.path != sweep.path || cache.at != sweep.at || cache.texel != sweep.texel || !(cache.shape == sweep.shape) || !(cache.limit == sweep.limit) ||
            cache.colour != colour) {
            cache.texture.reset();

            const smplhud::SweepColour tint = {dequantised(colour[0]), dequantised(colour[1]), dequantised(colour[2]), dequantised(colour[3])};
            auto                       sprite = smplhud::renderSweep(*sweep.path, sweep.at, sweep.shape, tint, sweep.texel, sweep.limit);
            if (sprite.empty() || !(sprite.box == box))
                return;

            cache.texture = g_pHyprRenderer->createTexture(DRM_FORMAT_ABGR8888, sprite.rgba.data(), uint32_t(sprite.width) * 4, {double(sprite.width), double(sprite.height)});
            cache.path    = sweep.path;
            cache.at      = sweep.at;
            cache.texel   = sweep.texel;
            cache.shape   = sweep.shape;
            cache.limit   = sweep.limit;
            cache.colour  = colour;
        }

        if (!cache.texture || !cache.texture->ok())
            return;

        scope.draw(cache.texture, target, damage, frame.alpha * sweep.intensity);
    }
}

CHudPassElement::CHudPassElement(SHudFrame frame) : m_frame(std::move(frame)) {
    ;
}

void clearHudStamps() {
    g_stamps.clear();
}

std::vector<UP<IPassElement>> CHudPassElement::draw() {
    if (!m_frame.layout)
        return {};

    auto&       render    = g_pHyprRenderer->m_renderData;
    const auto& layout    = *m_frame.layout;
    const CBox  savedClip = render.clipBox;
    const bool  hasClip   = savedClip.width != 0 && savedClip.height != 0;
    const auto  ox        = m_frame.window.x;
    const auto  oy        = m_frame.window.y;

    g_pHyprRenderer->blend(true);
    g_pHyprOpenGL->scissor(nullptr);

    for (const auto& ring : layout.rings) {
        const CBox inner = {ring.inner.x + ox, ring.inner.y + oy, double(ring.inner.w), double(ring.inner.h)};
        if (inner.width < 1 || inner.height < 1 || ring.borderSize < 1)
            continue;

        CBox clip = savedClip;
        if (ring.clipped) {
            clip = CBox{ring.clip.x + ox, ring.clip.y + oy, double(ring.clip.w), double(ring.clip.h)};
            if (hasClip)
                clip = clip.intersection(savedClip);
            if (clip.width < 1 || clip.height < 1)
                continue;
        }
        render.clipBox = clip;

        CHyprOpenGLImpl::SBorderRenderData data;
        data.round         = ring.round;
        data.roundingPower = m_frame.roundingPower;
        data.borderSize    = ring.borderSize;
        data.a             = ring.alpha * m_frame.alpha;
        data.outerRound    = ring.outerRound;

        const auto& paint = m_frame.paints[static_cast<size_t>(ring.paint)];
        if (paint.fading && !paint.from.m_colorsOkLabA.empty())
            g_pHyprOpenGL->renderBorder(inner, paint.from, paint.to, paint.lerp, data);
        else
            g_pHyprOpenGL->renderBorder(inner, paint.to, data);
    }

    render.clipBox = savedClip;

    for (const auto& fill : layout.fills) {
        const CBox box = {fill.box.x + ox, fill.box.y + oy, double(fill.box.w), double(fill.box.h)};
        if (box.width < 1 || box.height < 1)
            continue;

        CRegion damage = render.damage.copy().intersect(box);
        if (damage.empty())
            continue;

        const auto  rgba  = paintAt(m_frame.paints[static_cast<size_t>(fill.paint)], layout.sampleBox, fill.box.x + fill.box.w * 0.5, fill.box.y + fill.box.h * 0.5);
        const float alpha = rgba.a * fill.alpha * m_frame.alpha;
        if (alpha < 0.002F)
            continue;

        CHyprOpenGLImpl::SRectRenderData data;
        data.damage        = &damage;
        data.round         = fill.round;
        data.roundingPower = 1.F;
        g_pHyprOpenGL->renderRect(box, CHyprColor(rgba.r, rgba.g, rgba.b, alpha), data);
    }

    if (!layout.panel.empty())
        drawPanelLines(m_frame);

    for (const auto& sweep : m_frame.sweeps)
        drawSweep(m_frame, sweep);

    if (!layout.panel.empty()) {
        drawPanelDots(m_frame);
        drawGlyphs(m_frame);
    }

    g_pHyprOpenGL->scissor(nullptr);
    return {};
}

bool CHudPassElement::needsLiveBlur() {
    return false;
}

bool CHudPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CHudPassElement::boundingBox() {
    return m_frame.bounds;
}
