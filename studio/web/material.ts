// The game's physically based surface shader, as three.js's standard
// material patched to read Source 2's textures and parameters: a hemi
// octahedral normal with roughness in its blue channel, metalness in the
// color's alpha, a masked color tint, the scene object's tint and self
// illumination.
import * as THREE from 'three'
import type { Material } from '../studio.pb.js'
import type { TextureCache } from './textures.js'

// Params reads a material's parameters, by the name with or without the
// layer suffix 1 the game's shaders use.
class Params {
  constructor(private material: Material | undefined) {}

  // lookup finds a parameter in one of the material's maps.
  private lookup<T>(map: { [key: string]: T } | undefined, name: string) {
    return map?.[name] ?? map?.[`${name}1`] ?? map?.[name.replace(/1$/, '')]
  }

  int(name: string, fallback = 0): number {
    const value = this.lookup(this.material?.ints, name)
    return value === undefined ? fallback : Number(value)
  }

  float(name: string, fallback: number): number {
    return this.lookup(this.material?.floats, name) ?? fallback
  }

  // color reads an sRGB vector parameter as a linear color.
  color(name: string, fallback: number): THREE.Color {
    const v = this.lookup(this.material?.vectors, name)
    const color = new THREE.Color(fallback)
    if (v) color.setRGB(v.x ?? 0, v.y ?? 0, v.z ?? 0, THREE.SRGBColorSpace)
    return color
  }

  vector(name: string, fallback: THREE.Vector3): THREE.Vector3 {
    const v = this.lookup(this.material?.vectors, name)
    return v ? new THREE.Vector3(v.x ?? 0, v.y ?? 0, v.z ?? 0) : fallback
  }

  texture(name: string): string | undefined {
    return this.lookup(this.material?.textures, name)
  }
}

// placeholder returns a one-texel texture, so every sampler is bound from
// the first frame and a real texture swaps in without a recompile.
function placeholder(r: number, g: number, b: number, a: number) {
  const texture = new THREE.DataTexture(new Uint8Array([r, g, b, a]), 1, 1)
  texture.needsUpdate = true
  return texture
}

// The placeholders draw as a flat, rough, untinted, unlit, dielectric
// surface.
const white = placeholder(255, 255, 255, 255)
const flat = placeholder(128, 128, 204, 255)
const black = placeholder(0, 0, 0, 255)

// noColor is the surface color of a material without a color texture.
const noColor = 0xc9c1b4

// fragment is the patched shader's code, by the three.js chunk it replaces.
const fragment: Record<string, string> = {
  color_fragment: /* glsl */ `
    // Correct, tint and light the color as the game's shader does.
    vec3 s2Albedo = diffuseColor.rgb;
    float s2Mask = texture2D(s2TintMask, vMapUv).x;
    float s2Luma = dot(s2Albedo, vec3(0.2126, 0.7152, 0.0722));
    s2Albedo = mix(vec3(s2Luma), s2Albedo, s2Csb.y) * s2Csb.z;
    s2Albedo = mix(s2Albedo, s2Albedo * s2Tint, s2MaskTint ? s2Mask : 1.0);
    #if defined(USE_COLOR) || defined(USE_INSTANCING_COLOR)
      s2Albedo = mix(s2Albedo, s2Albedo * vColor.rgb,
        (s2MaskVertexTint ? s2Mask : 1.0) * s2VertexTintStrength);
    #endif
    vec3 s2Emissive = vec3(0.0);
    #ifdef S2_SELF_ILLUM
      float s2IllumMask = texture2D(s2SelfIllumMask, vMapUv).x;
      vec3 s2IllumColor = mix(s2IllumTint, s2IllumTint * s2Albedo, s2IllumAlbedo);
      s2Albedo = mix(s2Albedo, s2IllumColor, s2IllumMask * saturate(s2IllumScale));
      s2Emissive = s2Albedo * s2IllumMask * s2IllumScale;
    #endif
    diffuseColor.rgb = s2Albedo;

    // Opaque surfaces keep metalness in the color's alpha.
    #ifdef S2_METALNESS_TEXTURE
      float s2Metal = texture2D(s2Metalness, vMapUv).x;
    #else
      float s2Metal = sampledDiffuseColor.a;
      diffuseColor.a = opacity;
    #endif
  `,
  roughnessmap_fragment: /* glsl */ `
    float roughnessFactor = texture2D(normalMap, vNormalMapUv).b;
  `,
  metalnessmap_fragment: /* glsl */ `
    float metalnessFactor = s2Metal;
  `,
  normal_fragment_maps: /* glsl */ `
    // Unfold the hemi octahedral normal.
    vec2 s2Oct = texture2D(normalMap, vNormalMapUv).xy;
    vec2 s2Fold = vec2(s2Oct.x + s2Oct.y - 1.003922, s2Oct.x - s2Oct.y);
    vec3 mapN = normalize(vec3(s2Fold, 1.0 - abs(s2Fold.x) - abs(s2Fold.y)));
    mapN.y = -mapN.y;
    mapN.xy *= normalScale;
    normal = normalize(tbn * mapN);
  `,
  emissivemap_fragment: /* glsl */ `
    totalEmissiveRadiance += s2Emissive;
  `,
}

// declarations are the patched shader's own uniforms.
const declarations = /* glsl */ `
  uniform sampler2D s2TintMask;
  uniform sampler2D s2SelfIllumMask;
  uniform sampler2D s2Metalness;
  uniform vec3 s2Tint;
  uniform bool s2MaskTint;
  uniform bool s2MaskVertexTint;
  uniform float s2VertexTintStrength;
  uniform vec3 s2Csb;
  uniform vec3 s2IllumTint;
  uniform float s2IllumAlbedo;
  uniform float s2IllumScale;
`

// patch rewrites a standard material's fragment shader into the game's.
function patch(shader: THREE.WebGLProgramParametersWithUniforms) {
  let code = shader.fragmentShader.replace(
    'void main() {',
    `${declarations}\nvoid main() {`,
  )
  for (const [chunk, replacement] of Object.entries(fragment))
    code = code.replace(`#include <${chunk}>`, replacement)
  shader.fragmentShader = code
}

// tintScale is how much each texture tint mode scales the tint: the game's
// mode 2 doubles it, and mode 1's saturation-preserving tint is drawn as a
// plain one.
const tintScale = [1, 1, 2]

// createMaterial makes the draw material for a game material, drawing at
// once with placeholders while its textures load from cache.
export function createMaterial(
  material: Material | undefined,
  textures: TextureCache,
): THREE.MeshStandardMaterial {
  // Read the shader's features.
  const p = new Params(material)
  const translucent = p.int('F_TRANSLUCENT') !== 0
  const alphaTest = p.int('F_ALPHA_TEST') !== 0
  const selfIllum = p.int('F_SELF_ILLUM') !== 0
  const npr = p.int('F_USE_NPR_LIGHTING') !== 0

  // Only the physically based shader keeps metalness in an opaque color's
  // alpha; the environment shaders keep other data there, and overlays
  // blend by it onto the surface below.
  const shader = material?.shader ?? ''
  const overlay = shader.includes('overlay')
  const blends = translucent || overlay
  const metalnessTexture = !shader.startsWith('pbr') || blends || alphaTest

  // The color tint is sRGB, scaled by its mode.
  const tint = p.color('g_vColorTint1', 0xffffff)
  tint.multiplyScalar(tintScale[p.int('g_nTextureColorTintMode1')] ?? 1)

  // Each parameter the patched shader reads is a uniform.
  const uniforms = {
    s2TintMask: { value: white as THREE.Texture },
    s2SelfIllumMask: { value: black as THREE.Texture },
    s2Metalness: { value: black as THREE.Texture },
    s2Tint: { value: tint },
    s2MaskTint: { value: p.int('g_bMaskColorTint1', 1) !== 0 },
    s2MaskVertexTint: { value: p.int('g_bMaskVertexColorTint1', 1) !== 0 },
    s2VertexTintStrength: { value: p.float('g_fVertexColorStrength1', 1) },
    s2Csb: {
      value: p.vector(
        'g_vAlbedoContrastSaturationBrightness1',
        new THREE.Vector3(1, 1, 1),
      ),
    },
    s2IllumTint: { value: p.color('g_vSelfIllumTint1', 0xffffff) },
    s2IllumAlbedo: { value: p.float('g_flSelfIllumAlbedoFactor1', 1) },
    s2IllumScale: { value: p.float('g_flSelfIllumScale1', 1) },
  }

  // Patch a standard material that already binds a color and a normal map.
  const drawn = new THREE.MeshStandardMaterial({
    map: white,
    normalMap: flat,
    color: p.texture('g_tColor') ? 0xffffff : noColor,
    side:
      p.int('F_RENDER_BACKFACES') !== 0 ? THREE.DoubleSide : THREE.FrontSide,
  })
  drawn.defines = {}
  if (selfIllum) drawn.defines.S2_SELF_ILLUM = ''
  if (metalnessTexture) drawn.defines.S2_METALNESS_TEXTURE = ''
  drawn.onBeforeCompile = (shader) => {
    Object.assign(shader.uniforms, uniforms)
    patch(shader)
  }

  // Cut out or blend by the color's alpha.
  if (alphaTest) drawn.alphaTest = p.float('g_flAlphaTestReference1', 0.5)
  if (blends) {
    drawn.transparent = true
    drawn.depthWrite = false
    drawn.opacity = p.float('g_flOpacityScale1', 1)
  }
  if (overlay) {
    drawn.polygonOffset = true
    drawn.polygonOffsetFactor = -1
    drawn.polygonOffsetUnits = -4
  }

  // Swap each texture in as it loads.
  const swap = (
    name: string,
    srgb: boolean,
    set: (t: THREE.Texture) => void,
  ) => {
    const path = p.texture(name)
    if (path)
      void textures.get(path, srgb).then((texture) => texture && set(texture))
  }
  swap('g_tColor', true, (t) => (drawn.map = t))
  swap('g_tNormalRoughness', false, (t) => (drawn.normalMap = t))
  swap(npr ? 'g_tTintMaskRimLightMask' : 'g_tTintMask', false, (t) => {
    uniforms.s2TintMask.value = t
  })
  if (selfIllum)
    swap('g_tSelfIllumMask', false, (t) => (uniforms.s2SelfIllumMask.value = t))
  if (metalnessTexture)
    swap('g_tMetalness', false, (t) => (uniforms.s2Metalness.value = t))
  return drawn
}
