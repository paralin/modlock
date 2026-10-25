// Decodes block-compressed textures to RGBA8 for a GPU that cannot sample
// them, as the GPU would: BC1 (DXT1), BC3 (DXT5), BC4, BC5 and BC7.

// BlockDecoder decodes one block at src[offset] into 16 RGBA texels, row by
// row.
type BlockDecoder = (src: Uint8Array, offset: number, out: Uint8Array) => void

// decoders maps a texture format to its block decoder and the bytes in each
// block, by the format numbers textures.ts uses.
const decoders: Record<number, [BlockDecoder, number]> = {
  1: [decodeBC1, 8],
  2: [decodeBC3, 16],
  20: [decodeBC7, 16],
  21: [decodeBC5, 16],
  27: [decodeBC4, 8],
}

// canDecode reports whether the page can decode format itself.
export const canDecode = (format: number) => format in decoders

// decode decodes one mip of width by height texels, stored as blocks, to
// RGBA8.
export function decode(
  format: number,
  src: Uint8Array,
  width: number,
  height: number,
): Uint8Array {
  // Decode each block in turn into a scratch 4x4 of texels.
  const [block, size] = decoders[format]!
  const out = new Uint8Array(width * height * 4)
  const texels = new Uint8Array(64)
  const across = Math.ceil(width / 4)
  const down = Math.ceil(height / 4)
  for (let by = 0; by < down; by++) {
    for (let bx = 0; bx < across; bx++) {
      block(src, (by * across + bx) * size, texels)

      // Copy the texels that fall inside the mip; edge blocks overhang it.
      for (let y = 0; y < 4 && by * 4 + y < height; y++) {
        const row = ((by * 4 + y) * width + bx * 4) * 4
        const n = Math.min(4, width - bx * 4) * 4
        out.set(texels.subarray(y * 16, y * 16 + n), row)
      }
    }
  }
  return out
}

// colorBlock decodes the 8 byte color half of a BC1 or BC3 block. Only BC1
// has the three color mode with transparent black, picked by the endpoints'
// order.
function colorBlock(
  src: Uint8Array,
  at: number,
  out: Uint8Array,
  threeColor: boolean,
) {
  // Build the four color palette from the two 565 endpoints.
  const c0 = src[at]! | (src[at + 1]! << 8)
  const c1 = src[at + 2]! | (src[at + 3]! << 8)
  const rgb = (c: number) => [
    expand((c >> 11) & 31, 5),
    expand((c >> 5) & 63, 6),
    expand(c & 31, 5),
  ]
  const a = rgb(c0)
  const b = rgb(c1)
  const palette = [
    [...a, 255],
    [...b, 255],
    [0, 0, 0, 255],
    [0, 0, 0, 255],
  ]
  for (let i = 0; i < 3; i++) {
    if (c0 > c1 || !threeColor) {
      palette[2]![i] = ((2 * a[i]! + b[i]!) / 3) | 0
      palette[3]![i] = ((a[i]! + 2 * b[i]!) / 3) | 0
    } else {
      palette[2]![i] = ((a[i]! + b[i]!) / 2) | 0
      palette[3]![3] = 0
    }
  }
  for (let t = 0; t < 16; t++) {
    const index = (src[at + 4 + (t >> 2)]! >> ((t & 3) * 2)) & 3
    out.set(palette[index]!, t * 4)
  }
}

// channelBlock decodes an 8 byte BC4 channel block into channel of out.
function channelBlock(
  src: Uint8Array,
  at: number,
  out: Uint8Array,
  channel: number,
) {
  // Build the eight value palette from the two endpoints.
  const a = src[at]!
  const b = src[at + 1]!
  const values = [a, b]
  if (a > b) {
    for (let i = 1; i < 7; i++) values.push((((7 - i) * a + i * b) / 7) | 0)
  } else {
    for (let i = 1; i < 5; i++) values.push((((5 - i) * a + i * b) / 5) | 0)
    values.push(0, 255)
  }

  // Sixteen 3 bit indexes follow in 48 bits, low bits first.
  let low = 0
  let high = 0
  for (let i = 0; i < 3; i++) {
    low |= src[at + 2 + i]! << (8 * i)
    high |= src[at + 5 + i]! << (8 * i)
  }
  for (let t = 0; t < 16; t++) {
    const index = t < 8 ? (low >> (3 * t)) & 7 : (high >> (3 * (t - 8))) & 7
    out[t * 4 + channel] = values[index]!
  }
}

function decodeBC1(src: Uint8Array, at: number, out: Uint8Array) {
  colorBlock(src, at, out, true)
}

function decodeBC3(src: Uint8Array, at: number, out: Uint8Array) {
  colorBlock(src, at + 8, out, false)
  channelBlock(src, at, out, 3)
}

function decodeBC4(src: Uint8Array, at: number, out: Uint8Array) {
  out.fill(0)
  channelBlock(src, at, out, 0)
  for (let t = 0; t < 16; t++) out[t * 4 + 3] = 255
}

function decodeBC5(src: Uint8Array, at: number, out: Uint8Array) {
  out.fill(0)
  channelBlock(src, at, out, 0)
  channelBlock(src, at + 8, out, 1)
  for (let t = 0; t < 16; t++) out[t * 4 + 3] = 255
}

// BC7 tables, from the BPTC format specification.

// partitions2 holds each two subset partition's subset bit per texel.
const partitions2 = [
  0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800,
  0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000, 0xf710, 0x008e,
  0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666,
  0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc,
  0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996,
  0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c,
  0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744,
  0xee22,
]

// anchors2 is each two subset partition's second subset anchor texel.
const anchors2 = [
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2,
  2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2,
  2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
]

// partitions3 holds each three subset partition's two subset bits per
// texel.
const partitions3 = [
  0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050,
  0x5555a0a0, 0x5a5a5050, 0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090,
  0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054,
  0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
  0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414,
  0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424,
  0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50, 0x500aa550, 0xaaaa4444,
  0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
  0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580,
  0xaa141414, 0x96960000, 0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000,
  0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
]

// anchors3 is each three subset partition's anchor texel for subsets one
// and two.
const anchors3 = [
  [
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6,
    10, 5, 8, 8, 6, 8, 5, 15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15,
    15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
  ],
  [
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15,
    8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15,
    8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15,
    8,
  ],
]

// weights are the interpolation weights out of 64 for 2, 3 and 4 bit
// indexes.
const weights = [
  [0, 21, 43, 64],
  [0, 9, 18, 27, 37, 46, 55, 64],
  [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64],
]

// Mode is one BC7 mode's layout.
interface Mode {
  subsets: number
  partitionBits: number
  rotationBits: number
  selectionBits: number
  colorBits: number
  alphaBits: number
  // pBits is 1 for a p-bit per endpoint, shared for one per subset.
  pBits: number
  shared: boolean
  indexBits: [number, number]
}

// modes are the eight BC7 modes.
const modes: Mode[] = [
  [3, 4, 0, 0, 4, 0, 1, 0, 3, 0],
  [2, 6, 0, 0, 6, 0, 1, 1, 3, 0],
  [3, 6, 0, 0, 5, 0, 0, 0, 2, 0],
  [2, 6, 0, 0, 7, 0, 1, 0, 2, 0],
  [1, 0, 2, 1, 5, 6, 0, 0, 2, 3],
  [1, 0, 2, 0, 7, 8, 0, 0, 2, 2],
  [1, 0, 0, 0, 7, 7, 1, 0, 4, 0],
  [2, 6, 0, 0, 5, 5, 1, 0, 2, 0],
].map(([s, pb, rb, isb, cb, ab, p, sh, i0, i1]) => ({
  subsets: s!,
  partitionBits: pb!,
  rotationBits: rb!,
  selectionBits: isb!,
  colorBits: cb!,
  alphaBits: ab!,
  pBits: p!,
  shared: sh === 1,
  indexBits: [i0!, i1!],
}))

// bits reads count bits at bit offset at of a 16 byte block, low bits first.
function bits(src: Uint8Array, base: number, at: number, count: number) {
  let value = 0
  for (let i = 0; i < count; i++) {
    const bit = at + i
    value |= ((src[base + (bit >> 3)]! >> (bit & 7)) & 1) << i
  }
  return value
}

// expand widens a value of n bits to 8 by repeating its high bits.
const expand = (v: number, n: number) => {
  v <<= 8 - n
  return (v | (v >> n)) & 255
}

function decodeBC7(src: Uint8Array, at: number, out: Uint8Array) {
  // The mode is the number of zero bits before the first one.
  let mode = 0
  while (mode < 8 && bits(src, at, mode, 1) === 0) mode++
  if (mode === 8) {
    out.fill(0)
    return
  }
  const m = modes[mode]!

  // Read the mode's partition, rotation and index selection in order.
  let pos = mode + 1
  const read = (n: number) => {
    const v = bits(src, at, pos, n)
    pos += n
    return v
  }
  const partition = read(m.partitionBits)
  const rotation = read(m.rotationBits)
  const selection = read(m.selectionBits)

  // Read the endpoints channel by channel, each subset's pair in turn.
  const endpoints = 2 * m.subsets
  const ep = [0, 1, 2, 3].map(() =>
    Array.from({ length: endpoints }, () => 255),
  )
  for (let c = 0; c < 3; c++)
    for (let e = 0; e < endpoints; e++) ep[c]![e] = read(m.colorBits)
  if (m.alphaBits)
    for (let e = 0; e < endpoints; e++) ep[3]![e] = read(m.alphaBits)

  // Append the p-bits, one per endpoint or one per subset, then widen.
  const colorBits = m.colorBits + m.pBits
  const alphaBits = m.alphaBits + m.pBits
  const pbits = Array.from({ length: endpoints }, () => 0)
  if (m.pBits) {
    for (let s = 0; s < m.subsets; s++) {
      const a = read(1)
      pbits[2 * s] = a
      pbits[2 * s + 1] = m.shared ? a : read(1)
    }
  }
  for (let e = 0; e < endpoints; e++) {
    for (let c = 0; c < 3; c++)
      ep[c]![e] = expand((ep[c]![e]! << m.pBits) | pbits[e]!, colorBits)
    if (m.alphaBits)
      ep[3]![e] = expand((ep[3]![e]! << m.pBits) | pbits[e]!, alphaBits)
  }

  // Each texel's index is one bit shorter at its subset's anchor. Modes 4
  // and 5 carry a second index set, which follows the first.
  const [bits0, bits1] = m.indexBits
  let offset0 = pos
  let offset1 = pos + 16 * bits0 - 1
  for (let t = 0; t < 16; t++) {
    let subset = 0
    let anchor = 0
    if (m.subsets === 2) {
      subset = (partitions2[partition]! >> t) & 1
      anchor = subset ? anchors2[partition]! : 0
    } else if (m.subsets === 3) {
      subset = (partitions3[partition]! >>> (2 * t)) & 3
      anchor = subset ? anchors3[subset - 1]![partition]! : 0
    }
    const short = t === anchor ? 1 : 0
    const index0 = bits(src, at, offset0, bits0 - short)
    offset0 += bits0 - short
    let index1 = index0
    if (bits1) {
      index1 = bits(src, at, offset1, bits1 - short)
      offset1 += bits1 - short
    }

    // The selection bit swaps which index set drives color and alpha.
    const w0 = weights[bits0 - 2]!
    const w1 = bits1 ? weights[bits1 - 2]! : w0
    const fc = selection ? w1[index1]! : w0[index0]!
    const fa = selection ? w0[index0]! : w1[index1]!
    const texel = [0, 0, 0, 0]
    for (let c = 0; c < 4; c++) {
      const f = c === 3 ? fa : fc
      const a = ep[c]![2 * subset]!
      const b = ep[c]![2 * subset + 1]!
      texel[c] = (a * (64 - f) + b * f + 32) >> 6
    }

    // Rotation swaps alpha with red, green or blue.
    if (rotation)
      [texel[3], texel[rotation - 1]] = [texel[rotation - 1]!, texel[3]!]
    out.set(texel, t * 4)
  }
}
