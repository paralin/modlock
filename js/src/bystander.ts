// A bystander is a hero kept out of play while its player waits: in the
// stands, on a bench, or lined up for a photo. It cannot move, shoot, cast or
// zoom, and nothing in play can hurt it or target it with an ability, so it
// can stand beside a fight without touching it. Its player still turns, so a
// mod that pins the camera on the fight (playCamera with curve.hold tracks for
// target and distance) gives them a view that circles it.
//
// spectateFrom parks a bystander out of sight and lets its player fly the
// free camera, so a player who is out of the round watches it and keeps their
// hero, where player.spectate() would take it away.

import {
  type Angles,
  holdModifierState,
  type Player,
  type Vector,
} from './host.gen.js'

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

/**
 * baronessHotel is a spot in the Baroness Hotel on dl_midtown: the closed hall
 * under Midtown where the Doorman's ultimate holds its victim. The stock map's
 * triggers box the hall from (-3197, 4304, -1888) to (-668, 4792, -1144); the
 * spot is at the box's middle, above its floor, so a hero placed there drops
 * to the floor. No lane, path or ability reaches the hall, and no player
 * outside it sees in.
 */
export const baronessHotel: Vector = { x: -1932, y: 4548, z: -1700 }

/**
 * spectateFrom holds the player's hero as a bystander, parks it at park
 * (baronessHotel unless given), and gives the player the free camera from
 * position. It returns a function that ends the view: it returns the player
 * to their hero, releases the hold and puts the hero back where it stood. A
 * mod that respawns or moves the hero itself may skip the move back with
 * back(false). It returns undefined when the player has no hero.
 */
export function spectateFrom(
  player: Player,
  position: Vector,
  facing: Angles,
  park: Vector = baronessHotel,
): ((restore?: boolean) => void) | undefined {
  // Remember where the hero stood, then hold it and park it out of sight.
  const pawn = player.pawn()
  if (pawn === undefined || pawn.hero === 0) return undefined
  const stood = pawn.position
  const faced = pawn.eyeAngles
  holdBystander(pawn.entity)
  player.teleport(park, faced, { x: 0, y: 0, z: 0 })
  player.freeCamera(position, facing)

  // Ending the view undoes each step, the move back only when asked.
  return (restore = true) => {
    player.unwatch()
    holdBystander(pawn.entity, false)
    if (restore) player.teleport(stood, faced, { x: 0, y: 0, z: 0 })
  }
}
