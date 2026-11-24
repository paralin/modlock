// loadSpot stands a spot in the world. Nothing is compiled: each object
// spawns as a solid model on the map the world already runs, so any mode on
// that map can load a spot as its arena.
//
//   const spot = loadSpot(decodeSpot(bytes))
//   onFrame((frame) => spot.bounce(frame.movement))
//   ...
//   spot.clear()

import {
  type Angles,
  createModel,
  log,
  type MovementSample,
  type Spot,
  type SpotObject,
  trace,
  type Vector,
  type WorldObject,
} from 'modlock'

/** Placed is one spot object standing in the world. */
export interface Placed {
  readonly object: SpotObject
  readonly world: WorldObject
  /** entity is the model's entity handle, as traces report it. */
  readonly entity: number
}

/**
 * triggers is the trigger collision layer. Traces that look for placed
 * objects exclude it: spawn rooms and other volumes enclose the ground, and a
 * line starting inside one stops there.
 */
export const triggers = 1n << 2n

/** feetProbe is how far below a hero's origin the bounce trace reaches. */
const feetProbe = 8

/**
 * LoadedSpot is a spot standing in the world. It keeps the placed objects in
 * placement order, so spot() returns what a player sees.
 */
export class LoadedSpot {
  readonly map: string
  private placed: Placed[] = []
  private entities = new Map<number, Placed>()

  constructor(map: string) {
    this.map = map
  }

  /** objects returns the placed objects in placement order. */
  objects(): readonly Placed[] {
    return this.placed
  }

  /**
   * place spawns object as a solid model and adds it to the spot. A model
   * the game cannot place logs why and returns nothing.
   */
  place(object: SpotObject): Placed | undefined {
    // Create the object's solid model in the world.
    const world = createModel({
      resource: object.model,
      position: object.position,
      facing: object.facing,
      scale: object.scale,
      solid: true,
    })

    // Skip an object the game does not create.
    const entity = world?.entity()
    if (!world || entity === undefined) {
      world?.remove()
      log(`spot: skipped ${object.model}`)
      return undefined
    }

    // Index the placed object by its entity.
    const placed = { object, world, entity }
    this.placed.push(placed)
    this.entities.set(entity, placed)
    return placed
  }

  /** at returns the placed object whose entity a trace hit, if any. */
  at(entity: number): Placed | undefined {
    return this.entities.get(entity)
  }

  /** move puts a placed object at position and facing, keeping its order. */
  move(placed: Placed, position: Vector, facing: Angles): Placed {
    // Move the model; a record not in this spot or a failed move stays as it was.
    const index = this.placed.indexOf(placed)
    if (index < 0 || !placed.world.move(position, facing)) {
      return placed
    }

    // Replace the record with the moved object.
    const moved = { ...placed, object: { ...placed.object, position, facing } }
    this.placed[index] = moved
    this.entities.set(moved.entity, moved)
    return moved
  }

  /** remove takes a placed object out of the world and the spot. */
  remove(placed: Placed): void {
    // Find the record in this spot.
    const index = this.placed.indexOf(placed)
    if (index < 0) {
      return
    }

    // Remove the model and its record.
    placed.world.remove()
    this.placed.splice(index, 1)
    this.entities.delete(placed.entity)
  }

  /** clear removes every placed object. */
  clear(): void {
    for (const placed of this.placed) {
      placed.world.remove()
    }
    this.placed = []
    this.entities.clear()
  }

  /** spot returns the spot as it stands. */
  spot(): Spot {
    return {
      map: this.map,
      objects: this.placed.map((placed) => placed.object),
    }
  }

  /**
   * bounce launches each sampled hero who stands on a bouncing object. A
   * mode calls it with each frame's movement samples, for the heroes it
   * watches.
   */
  bounce(movement: readonly MovementSample[]): void {
    if (!this.placed.some((placed) => placed.object.bounce > 0)) {
      return
    }
    for (const sample of movement) {
      if (!sample.grounded || sample.velocity.z > 0) {
        continue
      }
      const feet = sample.position
      const hit = trace({
        start: { x: feet.x, y: feet.y, z: feet.z + feetProbe },
        end: { x: feet.x, y: feet.y, z: feet.z - feetProbe },
        exclude: triggers,
        ignore: [sample.pawn],
      })
      const bounce = hit
        ? (this.entities.get(hit.entity)?.object.bounce ?? 0)
        : 0
      if (bounce > 0) {
        sample.player.setVelocity({
          x: sample.velocity.x,
          y: sample.velocity.y,
          z: bounce,
        })
      }
    }
  }
}

/**
 * loadSpot places every object of spot and returns it standing. Objects the
 * game cannot place are skipped with a log line; the rest load.
 */
export function loadSpot(spot: Spot): LoadedSpot {
  const loaded = new LoadedSpot(spot.map)
  for (const object of spot.objects) {
    loaded.place(object)
  }
  return loaded
}
