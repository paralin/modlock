// Loads game textures into three.js as stored: block-compressed formats go
// to the GPU unchanged with their whole mip chain, so the browser never
// decodes a texel.
import * as THREE from 'three'
import { Texture } from '../studio.pb.js'
import { fetchBytes, fetchMessage, route } from './api.js'

// Unavailable is a texture this browser or server cannot draw, as opposed to
// one that failed to load.
export class Unavailable extends Error {}

// compressed maps a block-compressed texture format to the three.js format
// it uploads as unchanged, the WebGL extension that draws it, and the bytes
// in each 4x4 block.
const compressed: Record<
  number,
  [THREE.CompressedPixelFormat, string, number]
> = {
  1: [THREE.RGBA_S3TC_DXT1_Format, 'WEBGL_compressed_texture_s3tc', 8],
  2: [THREE.RGBA_S3TC_DXT5_Format, 'WEBGL_compressed_texture_s3tc', 16],
  19: [THREE.RGB_BPTC_UNSIGNED_Format, 'EXT_texture_compression_bptc', 16],
  20: [THREE.RGBA_BPTC_Format, 'EXT_texture_compression_bptc', 16],
  21: [THREE.RED_GREEN_RGTC2_Format, 'EXT_texture_compression_rgtc', 16],
  27: [THREE.RED_RGTC1_Format, 'EXT_texture_compression_rgtc', 8],
}

// formatNames names the texture formats the page knows.
const formatNames: Record<number, string> = {
  1: 'DXT1',
  2: 'DXT5',
  4: 'RGBA8888',
  19: 'BC6H',
  20: 'BC7',
  21: 'BC5',
  27: 'BC4',
  28: 'BGRA8888',
}

// formatName names a texture format.
export const formatName = (format: number) =>
  formatNames[format] ?? `Format ${format}`

// Loaded is a texture ready to draw and the size of its largest mip.
export interface Loaded {
  texture: THREE.Texture
  width: number
  height: number
}

// loadTexture loads a texture's mips from the largest no wider than limit
// down to the smallest, reading its header first when header is null.
// Color textures sample as sRGB; the others hold data.
export async function loadTexture(
  renderer: THREE.WebGLRenderer,
  name: string,
  header: Texture | null,
  limit: number,
  srgb: boolean,
  signal: AbortSignal,
): Promise<Loaded> {
  // Check that the format can be drawn before fetching any texels.
  header ??= await fetchMessage(Texture, route('texture', name), signal)
  const format = header.format ?? 0
  const block = compressed[format]
  if (!header.served)
    throw new Unavailable(`${formatName(format)} textures are not served yet`)
  if (block && !renderer.getContext().getExtension(block[1]))
    throw new Unavailable(
      `This browser's GPU cannot draw ${formatName(format)} textures`,
    )

  // Pick the first mip and fetch it and every smaller one in one range.
  const mips = header.mips ?? 1
  const size = (mip: number) => ({
    width: Math.max(1, (header.width ?? 0) >> mip),
    height: Math.max(1, (header.height ?? 0) >> mip),
  })
  let from = 0
  while (
    from < mips - 1 &&
    Math.max(size(from).width, size(from).height) > limit
  )
    from++
  const data = await fetchBytes(route(`mips/${from}`, name), signal)

  // Split the range, which runs smallest mip first, into levels largest
  // first.
  const levels: THREE.CompressedTexture['mipmaps'] = []
  let end = data.length
  for (let mip = from; mip < mips; mip++) {
    const { width, height } = size(mip)
    const bytes = block
      ? Math.ceil(width / 4) * Math.ceil(height / 4) * block[2]
      : width * height * 4
    levels.push({ data: data.subarray(end - bytes, end), width, height })
    end -= bytes
  }
  const { width, height } = levels[0]!

  // Upload block formats as stored and swizzle BGRA into RGBA.
  let texture: THREE.Texture
  if (block) {
    texture = new THREE.CompressedTexture(levels, width, height, block[0])
  } else {
    if (format === 28) {
      for (let i = 0; i < data.length; i += 4)
        [data[i], data[i + 2]] = [data[i + 2]!, data[i]!]
    }
    texture = new THREE.DataTexture(
      levels[0]!.data,
      width,
      height,
      THREE.RGBAFormat,
    )
    texture.mipmaps = levels
  }

  // Sample between mips when the chain reaches one texel, as WebGL needs.
  const last = levels[levels.length - 1]!
  const complete = last.width === 1 && last.height === 1
  texture.minFilter = complete
    ? THREE.LinearMipmapLinearFilter
    : THREE.LinearFilter
  texture.magFilter = THREE.LinearFilter

  // Keep the stored mips, repeat as the game does and keep the rows in order.
  texture.generateMipmaps = false
  texture.anisotropy = renderer.capabilities.getMaxAnisotropy()
  texture.wrapS = texture.wrapT = THREE.RepeatWrapping
  texture.flipY = false
  if (srgb) texture.colorSpace = THREE.SRGBColorSpace
  texture.needsUpdate = true
  return { texture, width, height }
}

// TextureCache loads each texture once per color space for one asset, at a
// size limit, and reports each one that cannot be drawn.
export class TextureCache {
  private loads = new Map<string, Promise<THREE.Texture | null>>()

  constructor(
    private renderer: THREE.WebGLRenderer,
    private limit: number,
    private signal: AbortSignal,
    private onMissing: (texture: string, reason: string) => void,
  ) {}

  // get loads a texture, or resolves null for one that cannot be drawn.
  get(name: string, srgb: boolean): Promise<THREE.Texture | null> {
    const key = `${srgb ? 's' : 'l'}:${name}`
    let load = this.loads.get(key)
    if (!load) {
      load = loadTexture(
        this.renderer,
        name,
        null,
        this.limit,
        srgb,
        this.signal,
      )
        .then(({ texture }) => texture)
        .catch((error: Error) => {
          if (!this.signal.aborted) this.onMissing(name, error.message)
          return null
        })
      this.loads.set(key, load)
    }
    return load
  }

  // dispose frees every texture loaded.
  dispose() {
    for (const load of this.loads.values())
      void load.then((texture) => texture?.dispose())
    this.loads.clear()
  }
}
