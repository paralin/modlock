// Replayed damage applies a hit that was recorded elsewhere, so a hero takes
// it as if the attacker had dealt it just now.

import { damage, type Player } from 'modlock'

/** ReplayedHit is one recorded hit. */
export interface ReplayedHit {
  /** victim is the player whose hero takes the hit. */
  victim: Player
  /**
   * attacker is the player credited with the hit, usually a puppet. Without
   * one the hit is a hazard that credits no one.
   */
  attacker?: Player
  /** amount is the damage before resistances. */
  amount: number
  /** headshot makes the hit land on the head. */
  headshot?: boolean
  /**
   * ability names the attacker's ability that dealt the hit, such as
   * citadel_ability_hornet_snipe. It is left out of the hit when the attacker
   * does not own it.
   */
  ability?: string
}

/**
 * replayDamage deals hit through the game's own damage, so resistances,
 * kill credit, hit effects and the Damage events apply. It returns false
 * when either hero is not alive to take part; the host logs why.
 */
export function replayDamage(hit: ReplayedHit): boolean {
  // Both heroes must be in the world.
  const victim = hit.victim.pawn()
  const attacker = hit.attacker?.pawn()
  if (!victim || (hit.attacker && !attacker)) return false

  // Credit the ability by its entity when the attacker owns it.
  const ability = hit.ability
    ? hit.attacker?.abilities().find((owned) => owned.name === hit.ability)
    : undefined
  return damage({
    victim: victim.entity,
    attacker: attacker?.entity,
    amount: hit.amount,
    hitGroup: hit.headshot ? 1 : undefined,
    ability: ability?.entity,
  })
}
