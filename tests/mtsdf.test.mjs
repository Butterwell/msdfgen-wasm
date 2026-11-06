import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import { createMsdfgen } from '../src/loader.js';

// CI supplies a redistributable system font. Run after building dist/msdfgen.{mjs,wasm}.
const fontPath = process.env.MTSDF_TEST_FONT;
test('MTSDF generation has true-distance alpha and an independent glyph-index interface', { skip: !fontPath }, async () => {
    const module = await createMsdfgen();
    const font = module.loadFont(new Uint8Array(await readFile(fontPath)));
    assert.ok(font, 'font loaded');
    try {
        assert.equal(typeof font.generateMtsdfGlyph, 'function');
        assert.equal(typeof font.generateMtsdfGlyphByIndex, 'function');
        const glyph = font.generateMtsdfGlyph(0x41, 64, 8);
        assert.ok(glyph, 'font contains capital A');
        assert.equal(glyph.rgba.length, 64 * 64 * 4);
        const alpha = new Set();
        for (let i = 3; i < glyph.rgba.length; i += 4) alpha.add(glyph.rgba[i]);
        assert.ok(alpha.size > 2, 'true distance alpha must not be constant');
        assert.ok(glyph.advance > 0);
        const index = font.glyphIndexForCodepoint(0x41);
        assert.ok(index > 0, 'cmap resolves capital A');
        const byIndex = font.generateMtsdfGlyphByIndex(index, 64, 8);
        assert.ok(byIndex);
        assert.deepEqual(byIndex.rgba, glyph.rgba, 'font-index and codepoint MTSDF agree');
        assert.equal(byIndex.advance, glyph.advance);
        assert.equal(font.glyphIndexForCodepoint(0xd800), 0);
        assert.equal(font.generateMtsdfGlyphByIndex(0, 64, 8), null);
        assert.equal(font.generateMtsdfGlyph(-1, 64, 8), null);
        assert.equal(font.generateMtsdfGlyph(0xd800, 64, 8), null);
        assert.equal(font.generateMtsdfGlyph(0x41, 0, 8), null);
        const old = font.generateGlyph(0x41, 64, 8);
        assert.ok(old, 'previous MSDF API is still supported');
        assert.equal(old.rgba[3], 255, 'old format retains its opaque alpha');
    } finally {
        font.delete();
    }
});
