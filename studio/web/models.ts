// A model's parts as the viewport draws them: each draw call's geometry,
// built once from buffers shared between calls, and each material, made
// once per asset.
import * as THREE from 'three'
import type {
  Buffer,
  DrawCall,
  Lighting,
  Material,
  Model,
} from '../studio.pb.js'
import { route } from './api.js'
import {
  buildGeometry,
  decodeBuffer,
  readVertices,
  type Vertices,
} from './geometry.js'
import { BakedLighting } from './lighting.js'
import { createMaterial } from './material.js'
import type { TextureCache } from './textures.js'

// Materials makes each named material once for an asset.
export class Materials {
  private made = new Map<string, THREE.MeshStandardMaterial>()

  // baked is the map's baked lighting, or null for an asset without.
  private baked: BakedLighting | null = null

  constructor(readonly textures: TextureCache) {}

  // bake lights every lightmapped material made after it with a map's baked
  // lighting; world is the group that places the map.
  bake(lighting: Lighting, world: THREE.Object3D) {
    this.baked = new BakedLighting(lighting, this.textures, world)
  }

  // get returns the draw material for a game material, in the variant that
  // reads vertex colors when the geometry has them and the baked lighting
  // when it has lightmap coordinates.
  get(
    name: string,
    material: Material | undefined,
    colored: boolean,
    lightmapped = false,
  ) {
    // Make each variant once and share it.
    const baked = lightmapped ? this.baked : null
    const key = `${colored ? 'c' : 'p'}${baked ? 'l' : ''}:${name}`
    let made = this.made.get(key)
    if (!made) {
      made = createMaterial(material, this.textures, baked)
      made.vertexColors = colored
      this.made.set(key, made)
    }
    return made
  }

  // dispose frees every material and texture.
  dispose() {
    for (const material of this.made.values()) material.dispose()
    this.made.clear()
    this.textures.dispose()
  }
}

// Call is one draw call of a model, by mesh and index, with the index it
// has across all the model's meshes.
export interface Call {
  mesh: number
  index: number
  flat: number
  call: DrawCall
}

// hasMeshes reports whether a model has any draw call to draw.
export const hasMeshes = (model: Model) =>
  (model.meshes ?? []).some((mesh) => (mesh.drawCalls?.length ?? 0) > 0)

// hasCollision reports whether a model has collision shapes.
export const hasCollision = (model: Model) => (model.collision?.shapes ?? 0) > 0

// collisionCounts counts a model's collision shapes and their triangles.
export const collisionCounts = (model: Model) => ({
  shapes: model.collision?.shapes ?? 0,
  triangles: (model.collision?.indices?.length ?? 0) / 12,
})

// collisionGeometry builds a model's collision triangles, or returns null
// for a model without.
export function collisionGeometry(model: Model): THREE.BufferGeometry | null {
  // A model without collision has no geometry.
  const { positions, indices } = model.collision ?? {}
  if (!positions?.length || !indices?.length) return null

  // Copy the bytes out, since a decoded field may start unaligned.
  const geometry = new THREE.BufferGeometry()
  const floats = new Float32Array(
    positions.slice().buffer,
    0,
    positions.length / 4,
  )
  geometry.setAttribute('position', new THREE.BufferAttribute(floats, 3))
  geometry.setIndex(
    new THREE.BufferAttribute(
      new Uint32Array(indices.slice().buffer, 0, indices.length / 4),
      1,
    ),
  )
  return geometry
}

// calls lists a model's draw calls in mesh order.
export function calls(model: Model): Call[] {
  const out: Call[] = []
  for (const [mesh, m] of (model.meshes ?? []).entries())
    for (const [index, call] of (m.drawCalls ?? []).entries())
      out.push({ mesh, index, flat: out.length, call })
  return out
}

// Parts builds a model's draw calls once each, over vertices read once for
// all the calls that share them.
export class Parts {
  private buffers = new Map<string, Promise<Uint8Array>>()
  private vertices = new Map<string, Promise<Vertices>>()
  private geometries = new Map<number, Promise<THREE.BufferGeometry | null>>()

  constructor(
    readonly name: string,
    readonly model: Model,
    private signal: AbortSignal,
  ) {}

  // geometry builds one draw call's geometry, or resolves null for a call
  // whose buffers are missing.
  geometry(c: Call): Promise<THREE.BufferGeometry | null> {
    let built = this.geometries.get(c.flat)
    if (!built) {
      built = this.build(c)
      this.geometries.set(c.flat, built)
    }
    return built
  }

  // build reads a call's vertices and indices, shared with the mesh's other
  // calls, and makes its geometry.
  private async build(c: Call) {
    // Find the call's vertex streams and index buffer.
    const mesh = this.model.meshes![c.mesh]!
    const vs = c.call.vertexBuffers?.length ? c.call.vertexBuffers : [0]
    const i = c.call.indexBuffer ?? 0
    const vbs = vs.map((v) => mesh.vertexBuffers?.[v])
    const ib = mesh.indexBuffers?.[i]
    if (!ib || vbs.some((vb) => !vb)) return null

    // Read the streams' vertices once, and the call's indices.
    const key = `${c.mesh}:${vs.join(',')}`
    let vertices = this.vertices.get(key)
    if (!vertices) {
      vertices = Promise.all(
        vs.map((v, n) => this.buffer(c.mesh, 'vertex', v, vbs[n]!)),
      ).then((data) =>
        readVertices(
          data.map((bytes, n) => ({ buffer: vbs[n]!, data: bytes })),
        ),
      )
      this.vertices.set(key, vertices)
    }
    const [read, indexData] = await Promise.all([
      vertices,
      this.buffer(c.mesh, 'index', i, ib),
    ])
    this.signal.throwIfAborted()
    return buildGeometry(read, ib, indexData, c.call)
  }

  // buffer fetches and decodes one buffer of a mesh once.
  private buffer(
    mesh: number,
    kind: 'vertex' | 'index',
    index: number,
    desc: Buffer,
  ) {
    const path = route(`buffer/${mesh}/${kind}/${index}`, this.name)
    let decoded = this.buffers.get(path)
    if (!decoded) {
      decoded = decodeBuffer(path, kind, desc, this.signal)
      this.buffers.set(path, decoded)
    }
    return decoded
  }

  // dispose frees every geometry built.
  dispose() {
    for (const built of this.geometries.values())
      void built.then((geometry) => geometry?.dispose()).catch(() => {})
    this.geometries.clear()
  }
}
