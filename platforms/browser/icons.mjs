// SPDX-License-Identifier: GPL-3.0-or-later
// The fighters' stock icons, for the page's fighter grid, taken from the
// player's own disc: none of Melee's art ships with the page.
//
// They are the 24x24 CI4 images of the stock icon texture animation in
// IfAll.dat (Stc_scemdls); image N is the default costume of icon slot N,
// the slot gm_80168B34 gives a character kind.

const u16 = (b, o) => (b[o] << 8) | b[o + 1];
const u32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;

async function read(disc, offset, length) {
  return new Uint8Array(await disc.slice(offset, offset + length).arrayBuffer());
}

// A file in the disc's root directory, by name.
async function discFile(disc, names) {
  const header = await read(disc, 0x424, 8);
  const fstAt = u32(header, 0), fstSize = u32(header, 4);
  const fst = await read(disc, fstAt, fstSize);
  const count = u32(fst, 8);
  const strings = count * 12;
  for (const want of names) {
    for (let i = 1; i < count; i++) {
      const e = i * 12;
      if (fst[e] !== 0) continue; // a directory
      let at = strings + (u32(fst, e) & 0xffffff), name = '';
      while (fst[at]) name += String.fromCharCode(fst[at++]);
      if (name === want) return read(disc, u32(fst, e + 4), u32(fst, e + 8));
    }
  }
  return null;
}

// A GX palette entry as RGBA.
function paletteColour(v, format) {
  if (format === 2) { // RGB5A3
    if (v & 0x8000) return [((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31, 255];
    return [((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 / 7];
  }
  if (format === 1) return [(v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255]; // RGB565
  return [v & 255, v & 255, v & 255, v >> 8]; // IA8
}

// A CI4 image (8x8 tiles, two pixels a byte) as a PNG data URL.
function decodeCI4(data, imageAt, tlutAt) {
  const pixels = u32(data, imageAt), w = u16(data, imageAt + 4), h = u16(data, imageAt + 6);
  const lut = u32(data, tlutAt), lutFormat = u32(data, tlutAt + 4);
  const canvas = document.createElement('canvas');
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext('2d');
  const out = ctx.createImageData(w, h);
  for (let ty = 0; ty < h / 8; ty++) {
    for (let tx = 0; tx < w / 8; tx++) {
      const tile = pixels + (ty * (w / 8) + tx) * 32;
      for (let p = 0; p < 64; p++) {
        const b = data[tile + (p >> 1)];
        const index = p & 1 ? b & 15 : b >> 4;
        const rgba = paletteColour(u16(data, lut + index * 2), lutFormat);
        out.data.set(rgba, ((ty * 8 + (p >> 3)) * w + tx * 8 + (p & 7)) * 4);
      }
    }
  }
  ctx.putImageData(out, 0, 0);
  return canvas.toDataURL();
}

// The icon slot for a character kind (gm_80168B34): Zelda and Sheik are split
// by the fighter, and every kind after Sheik is one down.
const SHEIK = 19;
const iconSlot = (ckind) => (ckind === SHEIK ? 25 : ckind > SHEIK ? ckind - 1 : ckind);

// Map of character kind to icon URL, or null when the disc does not have
// them where expected.
export async function stockIcons(disc) {
  const file = await discFile(disc, ['IfAll.usd', 'IfAll.dat']);
  if (!file) return null;
  const dataSize = u32(file, 4);
  const data = file.subarray(0x20, 0x20 + dataSize);
  const inData = (p) => p > 0 && p < dataSize && p % 4 === 0;
  // The texture animation: 130 images and palettes, the first a 24x24 CI4.
  for (let at = 0; at + 0x18 <= dataSize; at += 4) {
    const images = u16(data, at + 0x14);
    if (images < 26 || images !== u16(data, at + 0x16) || u32(data, at + 4) > 8) continue;
    const imageTable = u32(data, at + 0xc), tlutTable = u32(data, at + 0x10);
    if (!inData(imageTable) || !inData(tlutTable)) continue;
    const first = u32(data, imageTable);
    if (!inData(first) || u16(data, first + 4) !== 24 || u16(data, first + 6) !== 24 ||
        u32(data, first + 8) !== 8) continue;
    const icons = new Map();
    for (let ckind = 0; ckind < 26; ckind++) {
      const slot = iconSlot(ckind);
      icons.set(ckind, decodeCI4(data, u32(data, imageTable + slot * 4), u32(data, tlutTable + slot * 4)));
    }
    return icons;
  }
  return null;
}
