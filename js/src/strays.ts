// Strays lays down hero pawns that no player holds any more. A hero swap, a
// move to the spectators or a reload can leave the old hero standing in the
// world, and the game revives it with no player on its own respawn timer.
// Removing a hero pawn leaves its abilities and items networked, which closes
// clients, and the Kill input only kills it. An ability left on a pawn no
// player drives can still run, and the server aborts when one asks for the
// player's command, as Lash's Down Strike did after a swap. So a stray loses
// its abilities, is killed once, and has its respawn held off for good; a dead
// hero draws nothing.

import {
  damage,
  findEntities,
  fireInput,
  log,
  readField,
  readFieldList,
  writeField,
} from './host.gen.js'
import { players } from './players.js'

/** strayGraceSeconds is how long a hero pawn belongs to no player before it is laid down. */
export const strayGraceSeconds = 3

/** strayRespawnTime is the game time a stray's respawn is held at, past any session. */
const strayRespawnTime = 1e9

/** strayBlow is a hit no hero's resistances survive. */
const strayBlow = 1_000_000

/**
 * Strays finds and lays down stray hero pawns. A mod that swaps heroes keeps
 * one and calls sweep each frame with the game time; it looks once a second.
 */
export class Strays {
  /** since maps each stray pawn to when it was first seen with no player. */
  private since = new Map<number, number>()
  private sweptAt = -Infinity

  /**
   * sweep lays down each hero pawn that has belonged to no connected player
   * for strayGraceSeconds, at most once a second. The game sets a dead
   * pawn's respawn when it dies, so the hold repeats on each sweep.
   */
  sweep(now: number): void {
    // Look at most once a second.
    if (now - this.sweptAt < 1) {
      return
    }
    this.sweptAt = now

    // A pawn is held when any connected player's hero is it.
    const held = new Set<number>()
    for (const { player } of players()) {
      const pawn = player.pawn()
      if (pawn) {
        held.add(pawn.entity)
      }
    }

    // Lay a pawn down once it has belonged to no one for the grace period.
    const since = new Map<number, number>()
    for (const entity of findEntities('player')) {
      if (held.has(entity)) {
        continue
      }
      const first = this.since.get(entity) ?? now
      since.set(entity, first)
      if (now - first >= strayGraceSeconds) {
        layDown(entity)
      }
    }
    this.since = since
  }
}

/**
 * layDown strips a stray hero pawn of its abilities, kills it if it stands,
 * and keeps its respawn out of reach.
 */
function layDown(entity: number): void {
  // Strip the abilities first, so none runs on the pawn with no player.
  const abilities = readFieldList(
    entity,
    'CCitadelPlayerPawn',
    'm_CCitadelAbilityComponent.m_vecAbilities',
    'handle',
  )
  let stripped = 0
  for (const ability of abilities) {
    if (typeof ability === 'number' && fireInput(ability, 'Kill')) {
      stripped++
    }
  }
  if (stripped > 0) {
    log(`stripped ${stripped} abilities from a stray hero pawn ${entity}`)
  }

  // Kill it if it stands, then hold its respawn past any session.
  const health = readField(entity, 'CBaseEntity', 'm_iHealth', 'int32')
  if (typeof health === 'number' && health > 0) {
    damage({ victim: entity, amount: strayBlow })
    log(`laid down a stray hero pawn ${entity}`)
  }
  writeField(
    entity,
    'CCitadelPlayerPawn',
    'm_flRespawnTime',
    'float32',
    strayRespawnTime,
  )
}
