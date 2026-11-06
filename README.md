# @playcanvas/msdfgen-wasm

Upstream [Chlumsky/msdfgen](https://github.com/Chlumsky/msdfgen) (v1.13, MIT) compiled to **WebAssembly** — generates per-glyph multi-channel signed distance fields in **Node and the browser**. It's the generation core behind [`@playcanvas/font-tools`](https://github.com/playcanvas/font-tools).

This Butterwell fork extends PlayCanvas's Emscripten binding for true MTSDF and font-glyph-index generation. The underlying upstream msdfgen library remains pinned as a submodule and is not patched. CI builds the WASM and commits the artifact to the repo, so consumers install with no toolchain.

## Install

```bash
npm install @playcanvas/msdfgen-wasm
```

## Usage

```js
import { createMsdfgen } from '@playcanvas/msdfgen-wasm';

const msdfgen = await createMsdfgen();       // loads the .wasm (Node or browser)
const font = msdfgen.loadFont(ttfBytes);     // Uint8Array of a TTF/OTF

// codepoint, cell size (px), pxrange. null if the font lacks the glyph.
const glyph = font.generateGlyph(0x41, 64, 8);
// glyph: { width, height, rgba,                       // size x size RGBA atlas cell
//          advance, translateX, translateY,           // em units
//          boundsL, boundsB, boundsR, boundsT, range }
font.delete();
```

Glyphs are rendered **em-normalized** (`-emnormalize`, so 1 unit = 1 em, independent of the font's `unitsPerEm`) and autoframed into a `size`×`size` cell. Metrics are reported in em units; PlayCanvas's 32-unit em is applied downstream by `font-tools`.

### Overlapping contours

Fonts derived from variable masters (e.g. Roboto 3.x from Google Fonts, Bahnschrift, most Google statics) keep **overlapping and self-intersecting contours**: a stem drawn straight through a bowl, a cedilla poking into its base glyph. msdfgen takes each texel's sign from the nearest edge, so edges running inside the fill punch holes into the distance field at stroke junctions. Upstream fixes this with Skia's path ops (`-preprocess`), which would multiply the WASM size. Instead, `cpp/resolve-overlaps.cpp` performs the equivalent for the non-zero winding rule before edge coloring: contours are split where they cross, pieces inside the filled region are dropped, and the boundary is re-chained into clean contours. The same winding test also corrects contours that run the wrong way for msdfgen's fill-on-the-right convention (whole fonts such as Roboto Mono, mirrored composites such as Arial's `Я`, reversed accent components), which previously came out inverted or with large blob artifacts. Glyphs that need neither are left untouched, so their output is bit-identical to a plain msdfgen build.

## MTSDF fork extensions (Butterwell)

The unchanged `generateGlyph(codepoint, size, pxrange)` API still generates MSDF
(RGB distance, alpha 255). The fork adds:

```js
const mtsdf = font.generateMtsdfGlyph(0x41, 64, 8);
// RGBA bytes: RGB = multi-channel distances, A = true signed distance.
// Metrics and em normalization have the same meanings as generateGlyph.

const shapedGlyph = font.generateMtsdfGlyphByIndex(glyphIndex, 64, 8);
// glyphIndex is a font-specific glyph ID produced by shaping (e.g. HarfBuzz).
// It must not be confused with a Unicode code point.
```

These methods require rebuilding `dist/msdfgen.{mjs,wasm}` from the changed
C++ binding. The original WASM artifact will NOT have the new methods.
The MTSDF integration test uses a redistributable font and a freshly built
WASM module:

```bash
MTSDF_TEST_FONT=/path/to/font.ttf node --test tests/mtsdf.test.mjs
```

HarfBuzz integration remains future work: a shaping engine will supply
glyph IDs and positions; this package only generates glyph bitmaps/metrics.

## Building the WASM (maintainers)

```bash
git submodule update --init --recursive          # vendor/msdfgen (upstream, pinned)
emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build                               # emits dist/msdfgen.{mjs,wasm}
```

FreeType is provided by Emscripten's port (`-sUSE_FREETYPE=1`); msdfgen's variable-font, SVG and PNG paths are disabled (we only load fonts and emit raw RGBA). CI builds and commits `dist/` (see `.github/workflows/build.yml`).

## License

MIT — see [LICENSE](LICENSE).

The distributed `dist/msdfgen.wasm` statically includes:

- **msdfgen** © Viktor Chlumsky — MIT ([source](https://github.com/Chlumsky/msdfgen))
- **FreeType** — The FreeType Project License (FTL). *This software is based in part on the work of the FreeType Team* ([freetype.org](https://freetype.org)).
