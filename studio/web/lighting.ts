// A map's baked lighting as the shader reads it: the lightmaps of its
// indirect light and of each stationary light's shadows, and the stationary
// lights the game draws over them at runtime.
import * as THREE from 'three'
import { Light_Kind, type Lighting } from '../studio.pb.js'
import type { TextureCache } from './textures.js'

// maxLights is how many stationary lights the shader draws; a map with more
// keeps its brightest.
const maxLights = 32

// texel returns a one-texel texture, bound until a lightmap loads.
function texel(r: number, g: number, b: number, a: number) {
  const texture = new THREE.DataTexture(new Uint8Array([r, g, b, a]), 1, 1)
  texture.needsUpdate = true
  return texture
}

// brightness is the luminance of a light's color.
const brightness = (light: { color?: number[] }) => {
  const [r = 0, g = 0, b = 0] = light.color ?? []
  return Math.abs(0.2126 * r + 0.7152 * g + 0.0722 * b)
}

// BakedLighting holds the uniforms every lightmapped material of one map
// shares. Until the indirect light loads, lightmapped surfaces draw as the
// unlit ones do.
export class BakedLighting {
  // defines fix, when the shader compiles, how many lights it loops over,
  // the distance at which a light's brightness is its own, and how bright
  // the baked light is where reflections reach full strength.
  readonly defines: Record<string, string>

  readonly uniforms = {
    s2Baked: { value: false },
    s2Irradiance: { value: texel(0, 0, 0, 255) as THREE.Texture },
    s2Direction: { value: texel(128, 128, 128, 255) as THREE.Texture },
    s2Shadows: { value: texel(255, 255, 255, 255) as THREE.Texture },
    s2LightmapScale: { value: new THREE.Vector2(1, 1) },
    // s2Source turns three.js world space into the game's, and s2World
    // turns a direction in the game's back.
    s2Source: { value: new THREE.Matrix4() },
    s2World: { value: new THREE.Matrix3() },
    // Each light is its origin and kind, its direction and range, its
    // color, its cone's cosines, and its shadow channel as a mask.
    s2LightOrigin: { value: [] as THREE.Vector4[] },
    s2LightDirection: { value: [] as THREE.Vector4[] },
    s2LightColor: { value: [] as THREE.Vector3[] },
    s2LightCone: { value: [] as THREE.Vector2[] },
    s2LightShadow: { value: [] as THREE.Vector4[] },
  }

  constructor(
    lighting: Lighting,
    textures: TextureCache,
    world: THREE.Object3D,
  ) {
    // Read the lightmaps' scale and the transforms between the two spaces.
    const [u = 1, v = 1] = lighting.uvScale ?? []
    this.uniforms.s2LightmapScale.value.set(u, v)
    world.updateWorldMatrix(true, false)
    this.uniforms.s2Source.value.copy(world.matrixWorld).invert()
    this.uniforms.s2World.value.setFromMatrix4(world.matrixWorld)

    // Keep the brightest lights and pack each one's parameters.
    // A map without any draws one black light, as the arrays need one.
    const lights = [...(lighting.lights ?? [])]
      .sort((a, b) => brightness(b) - brightness(a))
      .slice(0, maxLights)
    if (!lights.length) lights.push({})
    this.defines = {
      S2_LIGHTS: String(lights.length),
      S2_UNIT: '100.0',
      S2_REFLECT: '4.0',
    }
    const u3 = this.uniforms
    for (const light of lights) {
      // Place and aim it, with its kind and range.
      const [x = 0, y = 0, z = 0] = light.origin ?? []
      const [dx = 0, dy = 0, dz = -1] = light.direction ?? []
      const [r = 0, g = 0, b = 0] = light.color ?? []
      u3.s2LightOrigin.value.push(
        new THREE.Vector4(x, y, z, light.kind ?? Light_Kind.SUN),
      )
      u3.s2LightDirection.value.push(
        new THREE.Vector4(dx, dy, dz, light.range ?? 0),
      )
      u3.s2LightColor.value.push(new THREE.Vector3(r, g, b))

      // Its cone as cosines, and its shadow channel.
      const cos = (degrees: number | undefined) =>
        Math.cos(THREE.MathUtils.degToRad(Math.min(degrees ?? 180, 180)))
      u3.s2LightCone.value.push(
        new THREE.Vector2(cos(light.inner), cos(light.outer)),
      )
      const mask = new THREE.Vector4()
      mask.setComponent(Math.min(light.shadow ?? 0, 3), 1)
      u3.s2LightShadow.value.push(mask)
    }

    // Swap each lightmap in as it loads; the indirect light turns baking on.
    const swap = (
      name: string | undefined,
      set: (t: THREE.Texture) => void,
    ) => {
      if (name)
        void textures.get(name, false).then((t) => t && set(lightmap(t)))
    }
    swap(lighting.irradiance, (t) => {
      u3.s2Irradiance.value = t
      u3.s2Baked.value = true
    })
    swap(lighting.direction, (t) => (u3.s2Direction.value = t))
    swap(lighting.shadows, (t) => (u3.s2Shadows.value = t))
  }
}

// lightmap clamps a lightmap at the atlas's edges.
function lightmap(texture: THREE.Texture) {
  texture.wrapS = texture.wrapT = THREE.ClampToEdgeWrapping
  texture.anisotropy = 1
  texture.needsUpdate = true
  return texture
}

// vertex is the lightmapped vertex shader's code: the lightmap coordinate
// and the position in the game's space.
export const lightmapVertex = {
  declarations: /* glsl */ `
    attribute vec2 lightmapUv;
    uniform vec2 s2LightmapScale;
    uniform mat4 s2Source;
    varying vec2 vS2Lightmap;
    varying vec3 vS2Position;
  `,
  project_vertex: /* glsl */ `
    #include <project_vertex>
    vS2Lightmap = lightmapUv * s2LightmapScale;
    vec4 s2Local = vec4(transformed, 1.0);
    #ifdef USE_INSTANCING
      s2Local = instanceMatrix * s2Local;
    #endif
    vS2Position = (s2Source * modelMatrix * s2Local).xyz;
  `,
}

// lightmapFragment is the lightmapped fragment shader's code, which replaces
// the environment's diffuse light with the baked light and draws each
// stationary light masked by its baked shadow.
export const lightmapFragment = {
  declarations: /* glsl */ `
    uniform bool s2Baked;
    uniform sampler2D s2Irradiance;
    uniform sampler2D s2Direction;
    uniform sampler2D s2Shadows;
    uniform mat4 s2Source;
    uniform mat3 s2World;
    uniform vec4 s2LightOrigin[S2_LIGHTS];
    uniform vec4 s2LightDirection[S2_LIGHTS];
    uniform vec3 s2LightColor[S2_LIGHTS];
    uniform vec2 s2LightCone[S2_LIGHTS];
    uniform vec4 s2LightShadow[S2_LIGHTS];
    varying vec2 vS2Lightmap;
    varying vec3 vS2Position;

    // s2Falloff is how a stationary light of the given kind dims toward
    // the fragment: inverse square from a unit distance, faded out by its
    // range, and shaped by its cone.
    float s2Falloff(int i, vec3 toLight) {
      // A sun does not dim.
      vec4 origin = s2LightOrigin[i];
      vec4 direction = s2LightDirection[i];
      if (origin.w == 0.0) return 1.0;

      // Fall off with distance, to none at the range.
      float d = length(toLight);
      float window = 1.0 - pow(min(d / max(direction.w, 1.0), 1.0), 4.0);
      float falloff = window * window * S2_UNIT * S2_UNIT / max(d * d, 1.0);

      // A spot lights the half space it faces; a point light its cone.
      float facing = dot(-toLight / max(d, 1e-4), direction.xyz);
      if (origin.w == 2.0) return falloff * max(facing, 0.0);
      vec2 cone = s2LightCone[i];
      return cone.y <= -0.999 ? falloff
        : falloff * smoothstep(cone.y, max(cone.x, cone.y + 1e-3), facing);
    }
  `,
  lights_fragment_maps: /* glsl */ `
    #include <lights_fragment_maps>
    if (s2Baked) {
      // Light the surface's normal, in the game's space, from the baked
      // light and its dominant direction.
      vec3 s2Normal = normalize(mat3(s2Source) *
        inverseTransformDirection(geometryNormal, viewMatrix));
      vec3 s2Dc = texture2D(s2Irradiance, vS2Lightmap).rgb;
      vec3 s2Dir = texture2D(s2Direction, vS2Lightmap).rgb * 2.0 - 1.0;
      irradiance += PI * s2Dc * max(0.0, 1.0 + dot(s2Dir, s2Normal));

      // The baked light replaces the environment's diffuse light, and dims
      // its reflections in shadow.
      float s2Luma = dot(s2Dc, vec3(0.2126, 0.7152, 0.0722));
      iblIrradiance = vec3(0.0);
      radiance *= saturate(s2Luma * S2_REFLECT);

      // Draw each stationary light through its baked shadow.
      vec4 s2Mask = texture2D(s2Shadows, vS2Lightmap);
      for (int i = 0; i < S2_LIGHTS; i++) {
        vec4 origin = s2LightOrigin[i];
        vec3 toLight = origin.w == 0.0 ? -s2LightDirection[i].xyz
          : origin.xyz - vS2Position;
        float shadow = dot(s2Mask, s2LightShadow[i]);
        IncidentLight s2Light;
        s2Light.direction = normalize((viewMatrix * vec4(s2World * normalize(toLight), 0.0)).xyz);
        s2Light.color = PI * s2LightColor[i] * shadow * s2Falloff(i, toLight);
        s2Light.visible = true;
        RE_Direct(s2Light, geometryPosition, geometryNormal, geometryViewDir,
          geometryClearcoatNormal, material, reflectedLight);
      }
    }
  `,
}
