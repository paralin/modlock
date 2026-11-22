// Strays kills hero pawns that no player holds any more. A hero swap, a
// move to the spectators or a reload can leave the old hero standing in the
// world, and the game revives it with no player on its own respawn timer.
// Removing a hero pawn leaves its abilities and items networked, which closes
// clients, and killing one alone lets the game revive it. So a stray is
// killed once and has its respawn held off for good; a dead hero draws
// nothing and casts nothing. The kill is a forced death: the game drops a
// damage hit on a disconnected player's hero. Each stray is tried once, and
// one that still stands is reported once and left alone.
//
// A pawn is held when a connected player's hero is it, or when its own
// controller's hero is it: a player still joining, or watching from the
// spectators, has a controller that names their hero before the player
// reports it. A disconnected controller's hero is held while its seat is
// away, since the server gives that seat back to its player and a returning
// client whose hero lies dead drops the connection. Only a pawn both checks
// disown is a stray; an ended seat's hero Modlock kills itself.

import {
  findEntities,
  killEntity,
  log,
  readField,
  writeField,
} from './host.gen.js'
import { holdBystander } from './bystander.js'
import { players } from './players.js'

/** strayGraceSeconds is how long a hero pawn belongs to no player before it is killed. */
export const strayGraceSeconds = 3

/** strayRespawnTime is the game time a stray's respawn is held at, past any session. */
const strayRespawnTime = 1e9

/**
 * Strays finds and kills stray hero pawns. A mod that swaps heroes keeps
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
  /**
   * killed holds the strays already killed, or tried, for as long as they
   * stay strays: each is tried once, then only has its respawn held.
   */
  private killed = new Set<number>()
  private sweptAt = -Infinity

  /**
   * sweep kills each hero pawn that has belonged to no player for
   * strayGraceSeconds, once per pawn, at most once a second. The game sets a
   * dead pawn's respawn when it dies, so the hold repeats on each sweep. A
   * pawn a player holds again is forgotten, and killed anew if it strays
   * again.
   */
  sweep(now: number): void {
    // Look at most once a second.
    if (now - this.sweptAt < 1) {
      return
    }
    this.sweptAt = now

    // A pawn is held when any connected player's hero is it.
    const held = new Set<number>()
    const away = new Set<number>()
    for (const { player, away: gone } of players()) {
      if (gone) {
        away.add(player.slot)
        continue
      }
      const pawn = player.pawn()
      if (pawn) {
        held.add(pawn.entity)
      }
    }

    // Kill a pawn once it has belonged to no one for the grace period.
    const since = new Map<number, number>()
    const gone = new Set<number>()
    const killed = new Set<number>()
    for (const entity of findEntities('player')) {
      if (held.has(entity) || ownHero(entity, away, this.gone, gone)) {
        continue
      }
      if (this.killed.has(entity)) {
        holdRespawn(entity)
        killed.add(entity)
        continue
      }
      const first = this.since.get(entity) ?? now
      since.set(entity, first)
      if (now - first >= strayGraceSeconds) {
        killStray(entity)
        killed.add(entity)
      }
    }
    this.since = since
    this.gone = gone
    this.killed = killed
  }
}

/**
 * ownHero reports whether the hero pawn entity is its controller's hero, for
 * a controller that is connected or whose slot is in away. A pawn whose
 * controller is gone, disconnected for good, or names another hero, is not.
 * It skips a controller in known as gone and adds each gone controller it
 * sees to seen.
 */
function ownHero(
  entity: number,
  away: ReadonlySet<number>,
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

  // Disconnecting and disconnected controllers may keep naming old heroes;
  // only an away seat's still holds one. A player controller's entity index
  // is its slot plus one.
  const connected = readField(
    controller,
    'CBasePlayerController',
    'm_iConnected',
    'int32',
  )
  if (
    (connected === 3 || connected === 4) &&
    !away.has((controller & 0x7fff) - 1)
  ) {
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
 * killStray kills a stray hero pawn if it stands and keeps its respawn out of
 * reach. Its abilities stay: killing ability entities on a live hero pawn
 * has ended the server process. A pawn the forced death leaves standing is
 * reported once; the sweep does not try it again.
 */
function killStray(entity: number): void {
  // Release any bystander hold, which makes the hero invulnerable.
  holdBystander(entity, false)
  const health = readField(entity, 'CBaseEntity', 'm_iHealth', 'int32')
  if (typeof health !== 'number' || health <= 0) {
    holdRespawn(entity)
    return
  }

  // Force the death, then hold the respawn the death set.
  killEntity({ victim: entity })
  holdRespawn(entity)
  const left = readField(entity, 'CBaseEntity', 'm_iHealth', 'int32')
  if (typeof left === 'number' && left > 0) {
    log(
      `stray hero pawn ${entity} still stands after a forced death; left alone`,
    )
    return
  }
  log(`killed a stray hero pawn ${entity}`)
}

/** holdRespawn holds the dead hero pawn's respawn past any session. */
function holdRespawn(entity: number): void {
  writeField(
    entity,
    'CCitadelPlayerPawn',
    'm_flRespawnTime',
    'float32',
    strayRespawnTime,
  )
}
