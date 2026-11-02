// A bystander is a hero kept out of play while its player waits: in the
// stands, on a bench, or lined up for a photo. It cannot move, shoot, cast or
// zoom, and nothing in play can hurt it or target it with an ability, so it
// can stand beside a fight without touching it. Its player still turns, so a
// mod that pins the camera on the fight (playCamera with curve.hold tracks for
// target and distance) gives them a view that circles it.

import { holdModifierState } from './host.gen.js'

/** bystanderStates are the modifier states that keep a hero out of play. */
export const bystanderStates: readonly string[] = [
  'MODIFIER_STATE_IMMOBILIZED',
  'MODIFIER_STATE_DISARMED',
  'MODIFIER_STATE_MELEE_DISABLED',
  'MODIFIER_STATE_SILENCED',
  'MODIFIER_STATE_INVULNERABLE',
  'MODIFIER_STATE_TECH_UNTARGETABLE_BY_ENEMIES',
  'MODIFIER_STATE_TECH_UNTARGETABLE_BY_TEAMMATES',
  'MODIFIER_STATE_DISPLACEMENT_IMMUNE',
  'MODIFIER_STATE_IGNORE_BULLETS',
  'MODIFIER_STATE_IGNORE_MELEE',
  'MODIFIER_STATE_IN_ABILITY_DISABLE_ZOOM',
]

/**
 * holdBystander keeps the hero pawn entity out of play, or with active false
 * gives it back. The states stay until released or the pawn is removed, so
 * a mod releases them before the hero plays again. It returns false when a
 * state did not take, such as on a pawn that is gone.
 */
export function holdBystander(entity: number, active = true): boolean {
  let held = true
  for (const state of bystanderStates) {
    held = holdModifierState(entity, state, active) && held
  }
  return held
}
