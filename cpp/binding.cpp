// Emscripten/embind binding around upstream msdfgen v1.13.
//
// Exposes a Font class to JS (see src/loader.ts). Each glyph is rendered em-normalized
// (1 unit = 1 em) and centered in a size x size cell, matching the Editor pipeline's
//   -emnormalize -scale (size/2) -autoframe -size <size> <size> -pxrange <pxrange>
// so that 1 em -> size/2 px (the shipped fonts use 64px cells == 2 em). Metrics
// (advance/translate/bounds) are reported in em units; @playcanvas/font-tools scales by 32.
//
// The autoframe translate formula below was validated analytically against the engine's
// golden arial.json (e.g. 'A' -> xoffset 21.33, yoffset 20.55).

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <cmath>
#include <string>
#include <vector>

#include "msdfgen.h"
#include "msdfgen-ext.h"
#include "resolve-overlaps.h"

using namespace emscripten;

namespace {
msdfgen::FreetypeHandle *g_ft = nullptr;
msdfgen::FreetypeHandle *freetype() {
    if (!g_ft) g_ft = msdfgen::initializeFreetype();
    return g_ft;
}
} // namespace

class Font {
public:
    // `bytes` is a JS Uint8Array of the TTF/OTF. Kept alive for the font's lifetime
    // because FreeType's FT_New_Memory_Face references (does not copy) the buffer.
    explicit Font(val bytes)
        : data_(convertJSArrayToNumberVector<unsigned char>(bytes)) {
        handle_ = msdfgen::loadFontData(freetype(),
            reinterpret_cast<const msdfgen::byte *>(data_.data()), static_cast<int>(data_.size()));
    }

    ~Font() { if (handle_) msdfgen::destroyFont(handle_); }

    bool ok() const { return handle_ != nullptr; }

    // Keep the historical MSDF API while adding genuine MTSDF generation.
    val generateGlyph(int codepoint, int size, double pxrange) {
        return generateByCodepoint(codepoint, size, pxrange, false);
    }

    // RGB = multichannel signed distance; A = true signed distance.
    val generateMtsdfGlyph(int codepoint, int size, double pxrange) {
        return generateByCodepoint(codepoint, size, pxrange, true);
    }

    // HarfBuzz supplies font glyph indices, not Unicode code points.
    val generateMtsdfGlyphByIndex(int glyphIndex, int size, double pxrange) {
        if (!handle_ || glyphIndex <= 0) return val::null();
        return generateByIndex(msdfgen::GlyphIndex(static_cast<unsigned int>(glyphIndex)), size, pxrange, true);
    }

private:
    val generateByCodepoint(int codepoint, int size, double pxrange, bool mtsdf) {
        if (!handle_ || codepoint < 0 || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return val::null();

        // Skip codepoints the font doesn't define (don't bake .notdef into the atlas).
        msdfgen::GlyphIndex gindex(0);
        if (!msdfgen::getGlyphIndex(gindex, handle_, static_cast<msdfgen::unicode_t>(codepoint)) ||
            gindex.getIndex() == 0) {
            return val::null();
        }
        return generateByIndex(gindex, size, pxrange, mtsdf);
    }

    val generateByIndex(msdfgen::GlyphIndex gindex, int size, double pxrange, bool mtsdf) {
        if (size <= 0 || size > 1024 || !std::isfinite(pxrange) || pxrange <= 0)
            return val::null();

        msdfgen::Shape shape;
        double advance = 0.0;
        if (!msdfgen::loadGlyph(shape, handle_, gindex, msdfgen::FONT_SCALING_EM_NORMALIZED, &advance)) {
            return val::null();
        }
        // Fonts derived from variable masters (e.g. Roboto 3.x) keep overlapping/self-intersecting
        // contours; msdfgen alone would read the edges inside the fill as boundary and punch
        // holes at stroke junctions. Resolve them first (upstream needs Skia for this). This also
        // reorients contours that run the wrong way for msdfgen, which would render inverted.
        pcmsdf::resolveOverlaps(shape);
        shape.normalize();
        msdfgen::edgeColoringSimple(shape, 3.0);

        const msdfgen::Shape::Bounds b = shape.getBounds();
        const bool hasInk = !shape.contours.empty() && b.l <= b.r && b.b <= b.t;

        const double scale = 0.5 * size;       // px per em -> 64px cell == 2 em
        const double cellEm = size / scale;    // == 2
        const double rangeEm = pxrange / scale;

        // Autoframe: center the glyph's em-space bounds within the cell.
        double tx = 0.0, ty = 0.0;
        if (hasInk) {
            tx = 0.5 * (cellEm - (b.l + b.r));
            ty = 0.5 * (cellEm - (b.b + b.t));
        }

        std::vector<unsigned char> rgba(static_cast<size_t>(size) * size * 4, 0);
        if (hasInk) {
            if (mtsdf) {
                std::vector<float> sdf(static_cast<size_t>(size) * size * 4);
                msdfgen::BitmapRef<float, 4> ref(sdf.data(), size, size);
                const msdfgen::Projection projection(
                    msdfgen::Vector2(scale, scale), msdfgen::Vector2(tx, ty));
                msdfgen::generateMTSDF(ref, shape, projection, msdfgen::Range(rangeEm));
                msdfgen::distanceSignCorrection(ref, shape, projection, msdfgen::FILL_NONZERO);

                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const float *px = ref(x, size - 1 - y);
                        const size_t o = (static_cast<size_t>(y) * size + x) * 4;
                        for (int c = 0; c < 4; ++c)
                            rgba[o + c] = msdfgen::pixelFloatToByte(px[c]);
                    }
                }
            } else {
            std::vector<float> sdf(static_cast<size_t>(size) * size * 3);
            msdfgen::BitmapRef<float, 3> ref(sdf.data(), size, size);
            msdfgen::generateMSDF(ref, shape,
                msdfgen::Projection(msdfgen::Vector2(scale, scale), msdfgen::Vector2(tx, ty)),
                msdfgen::Range(rangeEm));
            // Scanline pass (what the msdfgen CLI does by default when built without Skia): flip
            // any texel whose sign disagrees with the non-zero fill. A no-op for well-formed
            // glyphs; it cleans up stray wedges/streaks that cusps and near-degenerate edges
            // produce, and is the safety net when resolveOverlaps() could not re-chain a shape.
            msdfgen::distanceSignCorrection(ref, shape,
                msdfgen::Projection(msdfgen::Vector2(scale, scale), msdfgen::Vector2(tx, ty)),
                msdfgen::FILL_NONZERO);

            // Pack to top-down RGBA (msdfgen bitmaps are bottom-up by default).
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const float *px = ref(x, size - 1 - y);
                    const size_t o = (static_cast<size_t>(y) * size + x) * 4;
                    rgba[o + 0] = msdfgen::pixelFloatToByte(px[0]);
                    rgba[o + 1] = msdfgen::pixelFloatToByte(px[1]);
                    rgba[o + 2] = msdfgen::pixelFloatToByte(px[2]);
                    rgba[o + 3] = 255;
                }
            }
            }
        }

        // Copy the buffer into a JS Uint8Array (the C++ vector is freed on return).
        val out = val::object();
        out.set("width", size);
        out.set("height", size);
        out.set("rgba", val::global("Uint8Array").new_(typed_memory_view(rgba.size(), rgba.data())));
        out.set("advance", advance);
        out.set("translateX", tx);
        out.set("translateY", ty);
        out.set("boundsL", hasInk ? b.l : 0.0);
        out.set("boundsB", hasInk ? b.b : 0.0);
        out.set("boundsR", hasInk ? b.r : 0.0);
        out.set("boundsT", hasInk ? b.t : 0.0);
        out.set("range", rangeEm);
        return out;
    }

private:
    std::vector<unsigned char> data_;
    msdfgen::FontHandle *handle_ = nullptr;
};

EMSCRIPTEN_BINDINGS(msdfgen_module) {
    class_<Font>("Font")
        .constructor<val>()
        .function("ok", &Font::ok)
        .function("generateGlyph", &Font::generateGlyph)
        .function("generateMtsdfGlyph", &Font::generateMtsdfGlyph)
        .function("generateMtsdfGlyphByIndex", &Font::generateMtsdfGlyphByIndex);
}
