// Strays lays down hero pawns that no player holds any more. A hero swap, a
// move to the spectators or a reload can leave the old hero standing in the
// world, and the game revives it with no player on its own respawn timer.
// Removing a hero pawn leaves its abilities and items networked, which closes
// clients, and killing one alone lets the game revive it. So a stray is
// killed once and has its respawn held off for good; a dead hero draws
// nothing and casts nothing.
//
// A pawn is held when a connected player's hero is it, or when its own
// controller's hero is it: a player still joining, or watching from the
// spectators, has a controller that names their hero before the player
// reports it. Only a pawn both checks disown is a stray.

import { damage, findEntities, log, readField, writeField } from './host.gen.js'
import { holdBystander } from './bystander.js'
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
  /**
   * gone holds the controller handles strays name that no longer resolve. A
   * handle carries its entity's serial, so it never resolves again, and each
   * failed read logs on the host: a sweep reads it once.
   */
  private gone = new Set<number>()
  private sweptAt = -Infinity

  /**
   * sweep lays down each hero pawn that has belonged to no player for
   * strayGraceSeconds, at most once a second. The game sets a dead pawn's
   * respawn when it dies, so the hold repeats on each sweep.
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
    const gone = new Set<number>()
    for (const entity of findEntities('player')) {
      if (held.has(entity) || ownHero(entity, this.gone, gone)) {
        continue
      }
      const first = this.since.get(entity) ?? now
      since.set(entity, first)
      if (now - first >= strayGraceSeconds) {
        layDown(entity)
      }
    }
    this.since = since
    this.gone = gone
  }
}

/**
 * ownHero reports whether the hero pawn entity is its controller's hero. A
 * pawn whose controller is gone, or names another hero, is not. It skips a
 * controller in known as gone and adds each gone controller it sees to seen.
 */
function ownHero(
  entity: number,
  known: Set<number>,
  seen: Set<number>,
): boolean {
  // A controller must still hold this hero on behalf of a connection.
  const controller = readField(
    entity,
    'CBasePlayerPawn',
    'm_hController',
    'handle',
  )
  if (typeof controller !== 'number') {
    return false
  }
  if (known.has(controller)) {
    seen.add(controller)
    return false
  }

  // Disconnecting and disconnected controllers may keep naming old heroes.
  const connected = readField(
    controller,
    'CBasePlayerController',
    'm_iConnected',
    'int32',
  )
  if (connected === 3 || connected === 4) {
    return false
  }
  const hero = readField(
    controller,
    'CCitadelPlayerController',
    'm_hHeroPawn',
    'handle',
  )

  // A controller whose fields both fail to read no longer resolves.
  if (connected === undefined && hero === undefined) {
    seen.add(controller)
  }
  return hero === entity
}

/**
 * layDown kills a stray hero pawn if it stands and keeps its respawn out of
 * reach. Its abilities stay: killing ability entities on a live hero pawn
 * has ended the server process.
 */
function layDown(entity: number): void {
  // Kill it if it stands, then hold its respawn past any session.
  holdBystander(entity, false)
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
