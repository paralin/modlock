// Restore sets a hero to a snapshot of another hero's state for a fresh start:
// where it stood, its health, level, souls, build and cooldowns.
//
//   restoreHero(player, { position, facing, health: 520, maxHealth: 760, souls: 4200 },
//     (error) => { if (error) log(error) })

import {
  type Angles,
  type EquipmentTarget,
  type HeroTarget,
  type Player,
  type TimerTarget,
  type Vector,
} from './host.gen.js'

/**
 * HeroSnapshot is the state restoreHero gives a hero. Omitted parts keep the
 * hero's own.
 */
export interface HeroSnapshot {
  /** position is the hero's origin. */
  position: Vector
  /** facing is where the hero and its camera face. */
  facing: Angles
  /** health and maxHealth are the hero's health and its base maximum. */
  health: number
  maxHealth: number
  /**
   * level is the hero's level. The restore also sets the hero's souls to what
   * the level needs, since the game raises a level its souls pass and never
   * lowers one: a richer hero loses souls, from its wallet first, and a poorer
   * one gains them.
   */
  level?: number
  /**
   * souls is the souls the hero holds unspent, once the build is in place.
   * The build a snapshot lists is not paid for from them. They are set after
   * the level, so souls past the level's band raise the level again.
   */
  souls?: number
  /** upgradeBonuses are the three stat bonuses purchases add. */
  upgradeBonuses?: readonly number[]
  /** abilities are the hero's abilities in their slots, with their bought tiers. */
  abilities?: readonly EquipmentTarget[]
  /** items replace every item the hero owns; an empty list removes them. */
  items?: readonly EquipmentTarget[]
  /** timers are the charges and cooldowns of owned abilities and items. */
  timers?: readonly TimerTarget[]
}

/** waiting holds the callback of each slot's running restore. */
const waiting = new Map<
  number,
  { souls?: number; done?: (error: string | undefined) => void }
>()

/**
 * restoreHero gives player's live hero the snapshot over the next few
 * seconds, and calls done with undefined once the state has held, or with
 * the reason it did not. A new restore for the player replaces the running
 * one, which never calls done. It returns false when the restore could not
 * start. Shields are not restored.
 */
export function restoreHero(
  player: Player,
  snapshot: HeroSnapshot,
  done?: (error: string | undefined) => void,
): boolean {
  // The hero spawns its souls with the build, so they are set first and the
  // unspent balance is corrected when the restore ends.
  const target: HeroTarget = {
    position: snapshot.position,
    facing: snapshot.facing,
    health: snapshot.health,
    maxHealth: snapshot.maxHealth,
    level: snapshot.level,
    upgradeBonuses: snapshot.upgradeBonuses,
    abilities: snapshot.abilities,
    items: snapshot.items,
    replaceItems: snapshot.items !== undefined,
    timers: snapshot.timers,
  }
  if (snapshot.souls !== undefined) player.startingSouls(snapshot.souls)
  waiting.set(player.slot, { souls: snapshot.souls, done })
  if (player.restoreHero(target)) return true
  waiting.delete(player.slot)
  return false
}

/**
 * settleRestore finishes player's restore when the host reports its end: it
 * sets the souls the snapshot holds and calls the restore's callback. The
 * library calls it for each Restored event.
 */
export function settleRestore(player: Player, error: string | undefined): void {
  // Only a restore this module started has souls to settle.
  const restore = waiting.get(player.slot)
  if (!restore) return
  waiting.delete(player.slot)

  // Pay or refund the difference between the souls held and the snapshot's.
  const held = player.pawn()?.souls
  if (
    restore.souls !== undefined &&
    held !== undefined &&
    held !== restore.souls
  ) {
    player.adjustSouls(restore.souls - held, true)
  }
  restore.done?.(error)
}
