// ground finds the floor under a point with one trace straight down, the way
// a mode drops a hero, a marker or an effect onto the map:
//
//   const floor = ground(spawn) ?? spawn
//   const deep = ground(point, { above: 300, below: 900, layers: Layers.solid })

import { Layers } from './bits.js'
import { trace, type Vector } from './host.gen.js'

/** GroundOptions shape the trace ground runs; each one has a default. */
export interface GroundOptions {
  /** above is how far over the point the trace starts, 64 units by default. */
  readonly above?: number
  /** below is how far under the point the trace ends, 1024 units by default. */
  readonly below?: number
  /** layers is what counts as floor: solid and player clip by default. */
  readonly layers?: bigint
}

/**
 * ground returns the floor under point, searched from just above it. It
 * returns undefined when no floor answers in reach, when the trace starts
 * inside a wall, and before the world's collision is ready.
 */
export function ground(
  point: Vector,
  {
    above = 64,
    below = 1024,
    layers = Layers.solid | Layers.playerClip,
  }: GroundOptions = {},
): Vector | undefined {
  const hit = trace({
    start: { ...point, z: point.z + above },
    end: { ...point, z: point.z - below },
    layers,
  })
  return hit && !hit.startSolid ? hit.position : undefined
}
