// A bystander is a hero kept out of play while its player waits: in the
// stands, on a bench, or lined up for a photo. It cannot move, shoot, cast or
// zoom, and nothing in play can hurt it or target it with an ability, so it
// can stand beside a fight without touching it. Its player still turns, so a
// mod that pins the camera on the fight (playCamera with curve.hold tracks for
// target and distance) gives them a view that circles it.
//
// spectateFrom parks a hidden bystander above the map and lets its player fly
// the free camera, so a player who is out of the round watches it and keeps
// their hero, where player.spectate() would take it away.

import {
  type Angles,
  holdModifierState,
  type Player,
  type Vector,
} from 'modlock'

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
 * parkedStates hide a parked hero on top of bystanderStates: no model, no
 * health bar or name, no minimap icon, and no out of play area check, so the
 * game leaves a hero hanging above the map alone.
 */
export const parkedStates: readonly string[] = [
  'MODIFIER_STATE_DO_NOT_DRAW_MODEL',
  'MODIFIER_STATE_UNIT_STATUS_HIDDEN',
  'MODIFIER_STATE_INVISIBLE_ON_MINIMAP',
  'MODIFIER_STATE_IGNORE_OUT_OF_PLAY_AREA_CHECK',
]

/**
 * parkHeight is how far above where it stood spectateFrom parks a hero, above
 * the roofs of a map and out of every hero's reach.
 */
export const parkHeight = 4000

/**
 * spectateFrom takes the player's hero out of play and out of sight, and gives
 * the player the free camera from position. The hero is held as a bystander,
 * hidden by parkedStates, moved parkHeight above where it stood (or to park)
 * and frozen there, where gravity does not pull it, so it works on any map.
 * It returns a function that ends the view: it returns the player to their
 * hero, releases the hold and the freeze and puts the hero back where it
 * stood. A mod that respawns or moves the hero itself may skip the move back
 * with back(false). It returns undefined when the player has no hero.
 */
export function spectateFrom(
  player: Player,
  position: Vector,
  facing: Angles,
  park?: Vector,
): ((restore?: boolean) => void) | undefined {
  // Remember where the hero stood.
  const pawn = player.pawn()
  if (pawn === undefined || pawn.hero === 0) return undefined
  const stood = pawn.position
  const faced = pawn.eyeAngles

  // Hide it, park it and freeze it there, then fly the camera.
  hideParked(pawn.entity, true)
  player.teleport(park ?? { ...stood, z: stood.z + parkHeight }, faced, {
    x: 0,
    y: 0,
    z: 0,
  })
  player.freeze(true)
  player.freeCamera(position, facing)

  // Ending the view undoes each step, the move back only when asked.
  return (restore = true) => {
    player.unwatch()
    player.freeze(false)
    if (restore) player.teleport(stood, faced, { x: 0, y: 0, z: 0 })
    hideParked(pawn.entity, false)
  }
}

/** hideParked holds or releases bystanderStates and parkedStates together. */
function hideParked(entity: number, active: boolean): void {
  holdBystander(entity, active)
  for (const state of parkedStates) holdModifierState(entity, state, active)
}
