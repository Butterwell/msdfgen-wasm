/**
 * Loader for the Emscripten-built msdfgen module (Node + browser). See README for the
 * contract. The .wasm is located next to dist/msdfgen.mjs automatically.
 *
 * @typedef {object} Font
 * @property {(codepoint:number, size:number, pxrange:number) => (object|null)} generateGlyph
 *   Renders a glyph em-normalized; returns flat fields { width, height, rgba, advance,
 *   translateX, translateY, boundsL, boundsB, boundsR, boundsT, range } or null if absent.
 * @property {(codepoint:number, size:number, pxrange:number) => (object|null)} generateMtsdfGlyph
 *   RGBA MTSDF with true signed distance in alpha.
 * @property {(glyphIndex:number, size:number, pxrange:number) => (object|null)} generateMtsdfGlyphByIndex
 *   Uses font glyph indices from a shaping engine (e.g. HarfBuzz), not Unicode code points.
 * @property {(codepoint:number) => number} glyphIndexForCodepoint
 *   Returns the font cmap index or 0 for an unmapped Unicode code point.
 * @property {() => boolean} ok
 * @property {() => void} delete
 */

import createMsdfgenModule from '../dist/msdfgen.mjs';

let modulePromise = null;

/**
 * Instantiate the msdfgen WASM module.
 *
 * @param {{ moduleOverrides?: object }} [opts] - Optional Emscripten module overrides.
 * @returns {Promise<{ loadFont(bytes: Uint8Array | ArrayBuffer): (Font|null) }>}
 */
export async function createMsdfgen(opts = {}) {
    if (!modulePromise) modulePromise = createMsdfgenModule(opts.moduleOverrides || {});
    const Module = await modulePromise;
    return {
        loadFont(bytes) {
            const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
            const font = new Module.Font(u8);
            if (!font.ok()) {
                font.delete();
                return null;
            }
            return font;
        }
    };
}
