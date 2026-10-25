// Builds a compiled map from its World: every model it places, drawn as one
// instanced mesh per model, draw call and material, so each repeated prop
// costs one draw call however often it is placed.
import * as THREE from 'three'
import { Model, type Placement, type World } from '../studio.pb.js'
import { fetchMessage, route } from './api.js'
import { calls, Parts, type Call, type Materials } from './models.js'

// WorldStats counts what a map draws.
export interface WorldStats {
  models: number
  instances: number
  drawCalls: number
  triangles: number
}

// Instance is one placement of one draw call.
interface Instance {
  matrix: THREE.Matrix4
  // tint is the linear tint, or null for white.
  tint: THREE.Color | null
}

// Batch is every instance of one draw call drawn with one material.
interface Batch {
  call: Call
  material: string
  instances: Instance[]
}

// concurrency is how many models load at once.
const concurrency = 6

// identity is the transform of an untransformed placement.
const identity = new THREE.Matrix4()

// matrixOf reads a 3x4 row-major transform, or the identity for none.
function matrixOf(rows: number[] | undefined): THREE.Matrix4 {
  if (!rows || rows.length !== 12) return identity
  const r = rows
  return new THREE.Matrix4().set(
    ...(r.slice(0, 4) as [number, number, number, number]),
    ...(r.slice(4, 8) as [number, number, number, number]),
    ...(r.slice(8, 12) as [number, number, number, number]),
    0,
    0,
    0,
    1,
  )
}

// tintOf reads an sRGB tint as a linear color, or null for white.
function tintOf(tint: number[] | undefined): THREE.Color | null {
  if (!tint?.length) return null
  const [r = 1, g = 1, b = 1] = tint
  return new THREE.Color().setRGB(r, g, b, THREE.SRGBColorSpace)
}

// batchesOf groups a model's placements into batches. A placement with
// fragments draws each fragment's draw call; one without draws every call
// of the model's finest level of detail and default mesh groups.
function batchesOf(model: Model, placements: Placement[]): Batch[] {
  // Find the calls a whole placement draws.
  const all = calls(model)
  const defaults = model.defaultGroups ?? 0n
  const whole = all.filter(({ mesh }) => {
    const m = model.meshes![mesh]!
    const lod = ((m.lodMask ?? 1n) & 1n) !== 0n
    const group = defaults === 0n || ((m.groupMask ?? 0n) & defaults) !== 0n
    return lod && group
  })

  // Add each instance to the batch of its call and material.
  const batches = new Map<string, Batch>()
  const add = (
    call: Call,
    override: string | undefined,
    instance: Instance,
  ) => {
    // Find or start the batch of this call and material.
    const material = override ?? call.call.material ?? ''
    const key = `${call.flat}:${material}`
    let batch = batches.get(key)
    if (!batch) {
      batch = { call, material, instances: [] }
      batches.set(key, batch)
    }
    batch.instances.push(instance)
  }
  for (const placement of placements) {
    // Place the whole model, or each fragment.
    const matrix = matrixOf(placement.transform)
    const tint = tintOf(placement.tint)
    const overrides = placement.materials ?? {}
    if (!placement.fragments?.length) {
      for (const call of whole)
        add(call, overrides[call.flat], { matrix, tint })
      continue
    }
    for (const fragment of placement.fragments) {
      const call = all[fragment.drawCall ?? 0]
      if (call)
        add(call, overrides[call.flat], {
          matrix: matrix.clone().multiply(matrixOf(fragment.transform)),
          tint: tintOf(fragment.tint) ?? tint,
        })
    }
  }
  return [...batches.values()]
}

// buildWorld loads every model a world places and calls onMesh with each
// batch's instanced mesh as it is built, and onModel after each model.
// A model that cannot be read is left out.
export async function buildWorld(
  world: World,
  materials: Materials,
  signal: AbortSignal,
  onMesh: (mesh: THREE.InstancedMesh) => void,
  onModel: () => void,
): Promise<WorldStats> {
  // Group the placements by model.
  const names = world.models ?? []
  const byModel = names.map((): Placement[] => [])
  for (const placement of world.placements ?? [])
    byModel[placement.model ?? 0]?.push(placement)
  const stats: WorldStats = {
    models: names.length,
    instances: world.placements?.length ?? 0,
    drawCalls: 0,
    triangles: 0,
  }

  // Build one model's batches.
  const build = async (index: number) => {
    // Read the model and batch its placements.
    const name = names[index]!
    const model = await fetchMessage(Model, route('model', name), signal)
    const parts = new Parts(name, model, signal)
    const batches = batchesOf(model, byModel[index]!)

    // Make each batch's instanced mesh.
    await Promise.all(
      batches.map(async (batch) => {
        // Build the call's geometry; a call without buffers is left out.
        const geometry = await parts.geometry(batch.call)
        signal.throwIfAborted()
        if (!geometry) return
        const material = materials.get(
          batch.material,
          world.materials?.[batch.material] ??
            model.materials?.[batch.material],
          geometry.hasAttribute('color'),
          geometry.hasAttribute('lightmapUv'),
        )

        // Place and tint every instance.
        const { instances } = batch
        const mesh = new THREE.InstancedMesh(
          geometry,
          material,
          instances.length,
        )
        const tinted = instances.some((instance) => instance.tint)
        const white = new THREE.Color(1, 1, 1)
        for (const [i, instance] of instances.entries()) {
          mesh.setMatrixAt(i, instance.matrix)
          if (tinted) mesh.setColorAt(i, instance.tint ?? white)
        }

        // Count what it draws and hand it over.
        mesh.computeBoundingSphere()
        mesh.name = `${name}: ${batch.material}`
        stats.drawCalls++
        stats.triangles += ((geometry.index?.count ?? 0) / 3) * instances.length
        onMesh(mesh)
      }),
    )
  }

  // Load the models a few at a time, each worker taking the next.
  let next = 0
  const worker = async () => {
    while (next < names.length) {
      const index = next++
      await build(index).catch((error: Error) => {
        if (signal.aborted) throw error
      })
      onModel()
    }
  }
  await Promise.all(Array.from({ length: concurrency }, worker))
  return stats
}
