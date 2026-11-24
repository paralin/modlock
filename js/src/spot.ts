// A spot is a base map and the objects players placed on it, saved as the
// spot document. encodeSpot writes the document, whose bytes depend only on
// the spot, and decodeSpot reads it back:
//
//   const spot = decodeSpot(bytes)
//   save(encodeSpot({ ...spot, objects: [...spot.objects, crate] }))
//
// modlock/kit's loadSpot stands a spot in the world.

import * as pb from '../../mod/spot/spot.pb.js'
import { type Angles, type Vector } from './host.gen.js'

/** SpotObject is one solid object placed in a spot. */
export interface SpotObject {
  /** model is the model's path, which the mode precached. */
  readonly model: string
  /** position is the model's origin. */
  readonly position: Vector
  /** facing is the model's rotation. */
  readonly facing: Angles
  /** scale multiplies the model's size. */
  readonly scale: number
  /**
   * bounce launches a hero who stands on the object upward at this speed, in
   * units per second; zero makes an ordinary object.
   */
  readonly bounce: number
  /** placedBy is the Steam ID of the player who placed the object, or zero. */
  readonly placedBy: bigint
}

/** Spot is a base map and the objects placed on it, in placement order. */
export interface Spot {
  readonly map: string
  readonly objects: readonly SpotObject[]
}

/**
 * encodeSpot returns the spot document. Equal spots encode to equal bytes.
 * Positions and angles keep single precision.
 */
export function encodeSpot(spot: Spot): Uint8Array {
  const models = new Table<string>()
  const builders = new Table<bigint>()
  const objects = spot.objects.map((object) => ({
    model: models.index(object.model),
    ...object.position,
    ...object.facing,
    scale: object.scale === 1 ? 0 : object.scale,
    bounce: object.bounce,
    builder: object.placedBy === 0n ? 0 : builders.index(object.placedBy) + 1,
  }))
  return pb.Spot.toBinary({
    map: spot.map,
    models: models.values,
    builders: builders.values,
    objects,
  })
}

/** decodeSpot reads a spot document. */
export function decodeSpot(bytes: Uint8Array): Spot {
  const spot = pb.Spot.fromBinary(bytes)
  const models = spot.models ?? []
  const builders = spot.builders ?? []
  return {
    map: spot.map ?? '',
    objects: (spot.objects ?? []).map((object) => ({
      model: models[object.model ?? 0] ?? '',
      position: { x: object.x ?? 0, y: object.y ?? 0, z: object.z ?? 0 },
      facing: {
        pitch: object.pitch ?? 0,
        yaw: object.yaw ?? 0,
        roll: object.roll ?? 0,
      },
      scale: object.scale || 1,
      bounce: object.bounce ?? 0,
      placedBy: builders[(object.builder ?? 0) - 1] ?? 0n,
    })),
  }
}

/** Table lists distinct values in first-seen order. */
class Table<T> {
  readonly values: T[] = []
  private indexes = new Map<T, number>()

  /** index returns value's position, adding it when new. */
  index(value: T): number {
    let index = this.indexes.get(value)
    if (index === undefined) {
      index = this.values.push(value) - 1
      this.indexes.set(value, index)
    }
    return index
  }
}
