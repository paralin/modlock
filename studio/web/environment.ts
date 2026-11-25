// Environment layers use TEXCOORD4 for blend paint and COLOR for tint. Their
// color alpha is a tint mask; the packed texture holds emission and metalness.
import * as THREE from 'three'
import type { Material } from '../studio.pb.js'
import type { TextureCache } from './textures.js'

/** createEnvironmentLayers patches the chunks of a standard material for environment layers. */
export function createEnvironmentLayers(
  material: Material,
  textures: TextureCache,
): {
  count: number
  uniforms: Record<string, THREE.IUniform>
  declarations: string
  vertex: string
  chunks: Record<string, string>
  dispose: () => void
} {
  // Bind only authored layers, keeping single-layer environment materials intact.
  const count =
    material.shader === 'environment_blend.vfx'
      ? material.textures?.g_tColor3
        ? 3
        : 2
      : 1
  const uniforms: Record<string, THREE.IUniform> = {}
  const code: string[] = []
  const white = new THREE.DataTexture(
    new Uint8Array([255, 255, 255, 255]),
    1,
    1,
  )
  const flat = new THREE.DataTexture(new Uint8Array([128, 128, 204, 255]), 1, 1)
  const black = new THREE.DataTexture(new Uint8Array([0, 0, 0, 255]), 1, 1)
  for (const texture of [white, flat, black]) texture.needsUpdate = true

  // Read layer parameters without falling back from one layer to another.
  const number = (name: string, fallback: number) =>
    material.floats?.[name] ?? Number(material.ints?.[name] ?? fallback)
  const vector = (name: string, fallback: number[]) => {
    const value = material.vectors?.[name]
    return value
      ? [value.x ?? 0, value.y ?? 0, value.z ?? 0, value.w ?? 0]
      : fallback
  }
  const color = (name: string) => {
    const [r = 1, g = 1, b = 1] = vector(name, [1, 1, 1])
    return new THREE.Color().setRGB(r, g, b, THREE.SRGBColorSpace)
  }
  const bind = (
    name: string,
    path: string | undefined,
    srgb: boolean,
    fallback: THREE.Texture,
  ) => {
    const uniform = { value: fallback }
    uniforms[name] = uniform
    if (path)
      void textures.get(path, srgb).then((texture) => {
        if (texture) {
          uniform.value = texture
          const average = uniforms[name.replace(/Color$/, 'Average')]
          if (name.endsWith('Color') && average) {
            ;(average.value as THREE.Vector3).copy(
              (texture.userData.s2AverageColor as THREE.Vector3 | undefined) ??
                new THREE.Vector3(1, 1, 1),
            )
          }
        }
      })
  }

  // Each layer has independent texture coordinates, color, tint and packed data.
  for (let i = 1; i <= count; i++) {
    const prefix = `s2Layer${i}`
    const tint = color(`g_vColorTint${i}`)
    tint.multiplyScalar(
      [1, 1, 2][number(`g_nTextureColorTintMode${i}`, 0)] ?? 1,
    )
    uniforms[`${prefix}Tint`] = { value: tint }
    uniforms[`${prefix}Average`] = { value: new THREE.Vector3(1, 1, 1) }
    uniforms[`${prefix}EmissionScale`] = {
      value: number(`g_fLayerSelfIllumScale${i}`, 1),
    }
    uniforms[`${prefix}EmissionTint`] = {
      value: new THREE.Vector3(
        ...(vector(`g_vLayerSelfIllumTint${i}`, [1, 1, 1]).slice(0, 3) as [
          number,
          number,
          number,
        ]),
      ),
    }
    for (const [uniform, parameter] of [
      ['RoughnessAdjust', 'Roughness'],
      ['TintMaskAdjust', 'TintMask'],
      ['MetalnessAdjust', 'Metalness'],
    ]) {
      const values = vector(
        `g_vLayer${parameter}ContrastBrightness${i}`,
        [1, 0],
      )
      uniforms[`${prefix}${uniform}`] = {
        value: new THREE.Vector2(values[0], values[1]),
      }
    }
    uniforms[`${prefix}Csb`] = {
      value: new THREE.Vector3(
        ...(vector(
          `g_vAlbedoContrastSaturationBrightness${i}`,
          [1, 1, 1],
        ).slice(0, 3) as [number, number, number]),
      ),
    }
    uniforms[`${prefix}Transform`] = {
      value: new THREE.Vector4(
        ...(vector(`g_vScaleRotationTranslation${i}`, [1, 0, 0, 0]) as [
          number,
          number,
          number,
          number,
        ]),
      ),
    }
    uniforms[`${prefix}MaskTint`] = {
      value: number(`g_bMaskColorTint${i}`, 1) !== 0,
    }
    uniforms[`${prefix}VertexTint`] = {
      value:
        number(`g_fVertexColorStrength${i}`, 1) * number(`g_bModelTint${i}`, 1),
    }
    uniforms[`${prefix}MaskVertexTint`] = {
      value: number(`g_bMaskVertexColorTint${i}`, 1) !== 0,
    }
    uniforms[`${prefix}NormalStrength`] = {
      value: number(`g_fLayerNormalStrength${i}`, 1),
    }
    bind(`${prefix}Color`, material.textures?.[`g_tColor${i}`], true, white)
    bind(
      `${prefix}Normal`,
      material.textures?.[`g_tNormalRoughness${i}`],
      false,
      flat,
    )
    bind(`${prefix}Packed`, material.textures?.[`g_tPacked${i}`], false, black)
    code.push(`
      uniform sampler2D ${prefix}Color, ${prefix}Normal, ${prefix}Packed;
      uniform vec3 ${prefix}Tint, ${prefix}Csb, ${prefix}Average, ${prefix}EmissionTint;
      uniform vec2 ${prefix}RoughnessAdjust, ${prefix}TintMaskAdjust, ${prefix}MetalnessAdjust;
      uniform float ${prefix}EmissionScale;
      uniform vec4 ${prefix}Transform;
      uniform bool ${prefix}MaskTint, ${prefix}MaskVertexTint;
      uniform float ${prefix}VertexTint, ${prefix}NormalStrength;
      S2Surface s2Sample${i}(vec3 vertexTint) {
        vec2 uv = s2Transform(vMapUv, ${prefix}Transform);
        vec4 color = texture2D(${prefix}Color, uv);
        vec3 corrected = ((color.rgb - ${prefix}Average) * ${prefix}Csb.x + ${prefix}Average) * ${prefix}Csb.z;
        vec3 luminance = normalize(vec3(0.2125, 0.7154, 0.0721));
        vec3 albedo = mix(vec3(dot(corrected, luminance * luminance)), corrected, ${prefix}Csb.y);
        float tintMask = s2Adjust(color.a, ${prefix}TintMaskAdjust);
        albedo = mix(albedo, albedo * ${prefix}Tint, ${prefix}MaskTint ? tintMask : 1.0);
        albedo = mix(albedo, albedo * vertexTint, (${prefix}MaskVertexTint ? tintMask : 1.0) * ${prefix}VertexTint);
        vec3 normalRoughness = texture2D(${prefix}Normal, uv).rgb;
        vec2 oct = normalRoughness.xy;
        vec2 fold = vec2(oct.x + oct.y - 1.003922, oct.x - oct.y);
        vec3 normal = normalize(vec3(fold, 1.0 - abs(fold.x) - abs(fold.y)));
        normal.xy *= vec2(1.0, -1.0) * ${prefix}NormalStrength;
        vec2 packed = texture2D(${prefix}Packed, uv).rg;
        return S2Surface(albedo, normal, s2Adjust(normalRoughness.b, ${prefix}RoughnessAdjust), s2Adjust(packed.g, ${prefix}MetalnessAdjust), albedo * packed.r * ${prefix}EmissionScale * ${prefix}EmissionTint);
      }
    `)
    if (i === 1) continue

    // Reveal masks gate the raw blend paint; they are data rather than sRGB color.
    bind(
      `${prefix}Reveal`,
      material.textures?.[`g_tRevealMask${i}`],
      false,
      white,
    )
    uniforms[`${prefix}RevealTransform`] = {
      value: new THREE.Vector4(
        ...(vector(`g_vRevealScaleRotationTranslation${i}`, [1, 0, 0, 0]) as [
          number,
          number,
          number,
          number,
        ]),
      ),
    }
    uniforms[`${prefix}RevealOffset`] = {
      value: number(`g_flRevealOffset${i}`, 0),
    }
    uniforms[`${prefix}RevealSoftness`] = {
      value: number(`g_flRevealSoftness${i}`, 0.125),
    }
    uniforms[`${prefix}RevealInvert`] = {
      value: number(`g_bRevealInvert${i}`, 0) !== 0,
    }
    uniforms[`${prefix}RevealSecondary`] = {
      value: number(`g_bRevealUseSecondaryUv${i}`, 0) !== 0,
    }
    code.push(`
      uniform sampler2D ${prefix}Reveal;
      uniform vec4 ${prefix}RevealTransform;
      uniform float ${prefix}RevealOffset, ${prefix}RevealSoftness;
      uniform bool ${prefix}RevealInvert, ${prefix}RevealSecondary;
      float s2Weight${i}(float paint) {
        if (paint <= 0.00001) return 0.0;
        if (paint >= 1.0) return 1.0;
        vec2 uv = ${prefix}RevealSecondary ? vS2SecondaryUv : vMapUv;
        float reveal = texture2D(${prefix}Reveal, s2Transform(uv, ${prefix}RevealTransform)).r;
        if (${prefix}RevealInvert) reveal = 1.0 - reveal;
        float midpoint = 0.5 + ${prefix}RevealOffset * 0.5;
        float threshold = reveal < 0.5 ? reveal * 2.0 * midpoint : (reveal - 0.5) * 2.0 * (1.0 - midpoint) + midpoint;
        float softness = max(${prefix}RevealSoftness, 0.00001);
        threshold = clamp(threshold, 0.0001, 0.9999) * (1.0 - 2.0 * softness) + softness;
        return smoothstep(-softness, softness, paint - threshold);
      }
    `)
  }

  // Composite the authored layers before the standard lighting chunks run.
  return {
    count,
    dispose: () => {
      for (const texture of [white, flat, black]) texture.dispose()
    },
    uniforms,
    vertex: `attribute vec4 s2Blend; attribute vec2 s2SecondaryUv; attribute vec3 s2Tint;
      varying vec3 vS2Blend; varying vec2 vS2SecondaryUv; varying vec3 vS2Tint;`,
    declarations: `
      varying vec3 vS2Blend; varying vec2 vS2SecondaryUv; varying vec3 vS2Tint;
      struct S2Surface { vec3 albedo; vec3 normal; float roughness; float metalness; vec3 emission; };
      vec2 s2Transform(vec2 uv, vec4 transform) {
        float angle = radians(transform.y);
        return transform.x == 0.0 ? vec2(0.0) : mat2(cos(angle), sin(angle), -sin(angle), cos(angle)) * (uv * transform.x + transform.zw);
      }
      float s2Adjust(float value, vec2 adjustment) { return clamp((value - 0.5 + adjustment.y) * adjustment.x + 0.5, 0.0, 1.0); }
      ${code.join('\n')}
    `,
    chunks: {
      color_fragment: `
        vec3 vertexTint = vS2Tint * s2ModelTint;
        S2Surface s2Surface = s2Sample1(vertexTint);
        ${Array.from({ length: count - 1 }, (_, n) => {
          const i = n + 2
          return `S2Surface s2Next${i} = s2Sample${i}(vertexTint);
            float s2Blend${i} = s2Weight${i}(vS2Blend.${i === 2 ? 'x' : 'y'});
            s2Surface.albedo = mix(s2Surface.albedo, s2Next${i}.albedo, s2Blend${i});
            s2Surface.normal = mix(s2Surface.normal, s2Next${i}.normal, s2Blend${i});
            s2Surface.roughness = mix(s2Surface.roughness, s2Next${i}.roughness, s2Blend${i});
            s2Surface.metalness = mix(s2Surface.metalness, s2Next${i}.metalness, s2Blend${i});
            s2Surface.emission = mix(s2Surface.emission, s2Next${i}.emission, s2Blend${i});`
        }).join('\n')}
        diffuseColor.rgb = s2Surface.albedo * diffuse;
        #ifndef S2_SEE_THROUGH
          diffuseColor.a = opacity;
        #endif
      `,
      roughnessmap_fragment: 'float roughnessFactor = s2Surface.roughness;',
      metalnessmap_fragment: 'float metalnessFactor = s2Surface.metalness;',
      normal_fragment_maps:
        'normal = normalize(tbn * normalize(s2Surface.normal));',
      emissivemap_fragment:
        'totalEmissiveRadiance += s2Surface.emission * diffuse;',
    },
  }
}
