// Stragglers clears players who hold the server without playing. A human who
// stops loading keeps their slot as a connecting player until the network
// gives up, and the game keeps a disconnected player's hero standing in the
// world. Stragglers kicks a connection that has not finished joining by a
// deadline and kills the heroes Strays finds no player holding.
//
// It sees only the slots the host lists: an attempt the engine never put in
// the server has no slot, so no mod can see or kick it.

import { log, serverCommand } from './host.gen.js'
import { players } from './players.js'
import { Strays } from './strays.js'

/** stragglerConnectSeconds is how long a human may take to finish joining by default. */
export const stragglerConnectSeconds = 120

/** StragglersOptions configures Stragglers. */
export interface StragglersOptions {
  /**
   * connectSeconds is how long a human may stay connecting before the server
   * kicks them, stragglerConnectSeconds by default. Zero or less never kicks.
   */
  connectSeconds?: number
  /**
   * heroes kills the heroes no player holds, such as a disconnected
   * player's, as Strays does. It is true by default.
   */
  heroes?: boolean
}

/**
 * Stragglers kicks players stuck joining and kills heroes left without a
 * player. A mod keeps one and calls sweep each frame with the game time; it
 * looks once a second.
 */
export class Stragglers {
  private readonly connectSeconds: number
  private readonly strays: Strays | undefined
  /** joining maps each connecting human's slot to its generation and when it was first seen. */
  private joining = new Map<number, { generation: number; since: number }>()
  /** kicked holds the generation each slot had when this kicked it. */
  private readonly kicked = new Map<number, number>()
  private sweptAt = -Infinity

  constructor(options: StragglersOptions = {}) {
    this.connectSeconds = options.connectSeconds ?? stragglerConnectSeconds
    this.strays = options.heroes === false ? undefined : new Strays()
  }

  /**
   * sweep kicks each human connecting for longer than connectSeconds, once
   * per connection, and kills heroes no player holds, at most once a
   * second.
   */
  sweep(now: number): void {
    // Kill heroes left without a player; Strays paces itself.
    this.strays?.sweep(now)

    // Look for stuck joins at most once a second.
    if (now - this.sweptAt < 1) {
      return
    }
    this.sweptAt = now
    if (this.connectSeconds <= 0) {
      return
    }

    // Time each human still joining from the first sweep that saw them.
    const joining = new Map<number, { generation: number; since: number }>()
    for (const connection of players()) {
      if (connection.bot || connection.ready) {
        continue
      }
      const slot = connection.player.slot
      const seen = this.joining.get(slot)
      const since =
        seen?.generation === connection.generation ? seen.since : now
      joining.set(slot, { generation: connection.generation, since })
      if (now - since < this.connectSeconds) {
        continue
      }

      // Kick each stuck connection once.
      if (this.kicked.get(slot) === connection.generation) {
        continue
      }
      this.kicked.set(slot, connection.generation)
      const name = connection.name
      if (name === '' || name.includes('"')) {
        log(`slot ${slot} stuck joining, but its name cannot be kicked`)
        continue
      }
      log(`kicked ${name}: still joining after ${this.connectSeconds} s`)
      serverCommand(`kick "${name}"`)
    }
    this.joining = joining
  }
}
