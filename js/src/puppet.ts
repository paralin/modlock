// A puppet is a hero that follows a recorded track: where a hero stood, looked
// and how much health it had at a fixed step, played back with the controls of
// a player. It drives a bot added with addBot or any other player's hero.
//
//   const puppet = Puppet.spawn({ name: 'Replay', team: 2, hero: 'hero_wraith' }, track)
//   puppet?.setGhost(true)
//   puppet?.play()
//
// The library moves every playing puppet once per frame from the game clock.

import {
  addBot,
  type Angles,
  type BotOptions,
  holdModifierState,
  Player,
  type Vector,
  writeField,
} from './host.gen.js'
import { players } from './players.js'

/** ticksPerSecond is the game's tick rate; a track's steps count ticks. */
export const ticksPerSecond = 64

/**
 * Track is a recorded hero path in columns: sample i was taken at tick
 * i * stepTicks. The columns of x, y, z and yaw are as long as the track.
 */
export interface Track {
  /** stepTicks is the ticks between two samples. */
  stepTicks: number
  /** x, y and z are the hero's origin, at its feet. */
  x: number[]
  y: number[]
  z: number[]
  /** yaw is where the hero faces, in degrees. */
  yaw: number[]
  /** pitch is where the hero looks up or down, in degrees; level when absent. */
  pitch?: number[]
  /** health is the hero's health; the hero's own is kept when absent. */
  health?: number[]
}

/** Pose is a track read at one tick. */
export interface Pose {
  position: Vector
  facing: Angles
  /** velocity is the motion between the two samples the pose lies in. */
  velocity: Vector
  /** health is absent when the track records none. */
  health?: number
}

/** trackTicks is the tick of the track's last sample. */
export function trackTicks(track: Track): number {
  return Math.max(track.x.length - 1, 0) * track.stepTicks
}

/** lerp blends a toward b by t. */
function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t
}

/** lerpAngle blends the angle a toward b by t along the shorter way round. */
function lerpAngle(a: number, b: number, t: number): number {
  const delta = ((((b - a) % 360) + 540) % 360) - 180
  return a + delta * t
}

/**
 * poseAt reads track at tick, blending the two samples around it: positions
 * and health linearly, angles along the shorter arc. A tick past either end
 * reads that end. It returns undefined for a track without samples.
 */
export function poseAt(track: Track, tick: number): Pose | undefined {
  // Find the samples around the tick and how far between them it falls.
  const last = track.x.length - 1
  if (last < 0) return undefined
  const at = Math.min(Math.max(tick / track.stepTicks, 0), last)
  const i = Math.min(Math.floor(at), Math.max(last - 1, 0))
  const j = Math.min(i + 1, last)
  const t = at - i

  // Blend the columns, and take the velocity from the segment's slope.
  const seconds = track.stepTicks / ticksPerSecond
  const blend = (column: number[]) =>
    lerp(sample(column, i), sample(column, j), t)
  const slope = (column: number[]) =>
    (sample(column, j) - sample(column, i)) / seconds
  const still = i === j
  return {
    position: { x: blend(track.x), y: blend(track.y), z: blend(track.z) },
    facing: {
      pitch: track.pitch ? blend(track.pitch) : 0,
      yaw: lerpAngle(sample(track.yaw, i), sample(track.yaw, j), t),
      roll: 0,
    },
    velocity: still
      ? { x: 0, y: 0, z: 0 }
      : { x: slope(track.x), y: slope(track.y), z: slope(track.z) },
    health: track.health && blend(track.health),
  }
}

/** sample reads column at index, zero where it is short. */
function sample(column: number[], index: number): number {
  return column[index] ?? 0
}

/**
 * PuppetState is where playback stands: paused holds the hero at the tick,
 * playing advances it with the game clock, and ended is a play that reached
 * the track's last tick, which seek or play from a lower tick restarts.
 */
export type PuppetState = 'paused' | 'playing' | 'ended'

/** PuppetOptions tunes a puppet. */
export interface PuppetOptions {
  /**
   * snapUnits is the distance, in world units, beyond which the hero
   * teleports to the pose instead of gliding to it: after a seek, or across a
   * gap in the recording. The default is 200.
   */
  snapUnits?: number
  /** ghost starts the puppet as a ghost. */
  ghost?: boolean
}

/**
 * ghostStates are the modifier states of a ghost: it takes no damage, enemies
 * and units neither target nor count it, and its unit status stays hidden.
 */
export const ghostStates = [
  'MODIFIER_STATE_INVULNERABLE',
  'MODIFIER_STATE_NO_TARGET',
  'MODIFIER_STATE_IGNORED_BY_UNIT_FILTER',
  'MODIFIER_STATE_IGNORED_BY_NPC_TARGETING',
  'MODIFIER_STATE_INVISIBLE_ON_MINIMAP',
  'MODIFIER_STATE_UNIT_STATUS_HIDDEN',
]

/** ghostAlpha is how opaque a ghost is drawn, from 0 to 255. */
const ghostAlpha = 110

/** kRenderTransColor is the render mode that blends a model by its color alpha. */
const renderTransColor = 1

/** live holds the puppets the library moves each frame. */
const live = new Set<Puppet>()

/** longestFrame bounds the time a frame advances playback, in seconds. */
const longestFrame = 0.25

/**
 * advancePuppets moves every playing puppet to the game clock at seconds.
 * The library calls it once per frame.
 */
export function advancePuppets(seconds: number): void {
  for (const puppet of live) puppet.advance(seconds)
}

/**
 * Puppet plays a Track on a hero. Playback starts paused at tick 0; the hero
 * moves only while the puppet is playing or after a seek.
 */
export class Puppet {
  private tickAt = 0
  private rateNow = 1
  private current: PuppetState = 'paused'
  /** clock is the game time of the last frame, unset until one arrives. */
  private clock: number | undefined
  /** drawn is whether the hero needs placing at the current tick. */
  private drawn = false
  private ghostNow = false
  private dead = false
  /** looked is the entity and ghost setting syncLook last wrote. */
  private looked?: { entity: number; ghost: boolean }
  private lastHealth: number | undefined
  private readonly snapUnits: number

  /**
   * The puppet drives player's hero along track. It owns the hero's placement
   * and, when the track records health, its health.
   */
  constructor(
    readonly player: Player,
    readonly track: Track,
    options: PuppetOptions = {},
  ) {
    this.snapUnits = options.snapUnits ?? 200
    live.add(this)
    if (options.ghost) this.setGhost(true)
  }

  /**
   * spawn adds a bot and returns a puppet for it, or undefined when no bot
   * could join. The bot appears at the track's first pose.
   */
  static spawn(
    bot: BotOptions,
    track: Track,
    options?: PuppetOptions,
  ): Puppet | undefined {
    const start = poseAt(track, 0)
    const player = addBot({ ...bot, position: bot.position ?? start?.position })
    return player && new Puppet(player, track, options)
  }

  /** state is where playback stands. */
  get state(): PuppetState {
    return this.current
  }

  /** tick is the playback position, in track ticks. */
  get tick(): number {
    return this.tickAt
  }

  /** rate is the playback speed: 1 is real time, 0.5 half speed, negative runs backward. */
  get rate(): number {
    return this.rateNow
  }

  set rate(rate: number) {
    this.rateNow = rate
  }

  /** isGhost reports whether the puppet is a ghost. */
  get isGhost(): boolean {
    return this.ghostNow
  }

  /** play starts or resumes playback; from the end it restarts at tick 0. */
  play(): void {
    if (this.current === 'ended' && this.rateNow >= 0) this.seek(0)
    this.current = 'playing'
  }

  /** pause holds the hero where it stands. */
  pause(): void {
    this.current = 'paused'
  }

  /**
   * seek moves playback to tick, clamped to the track, and places the hero
   * there on the next frame. Playback keeps its state, except that an ended
   * puppet paused at the new tick.
   */
  seek(tick: number): void {
    this.tickAt = Math.min(Math.max(tick, 0), trackTicks(this.track))
    this.drawn = false
    this.lastHealth = undefined
    if (this.current === 'ended') this.current = 'paused'
  }

  /**
   * setGhost draws the hero translucent and holds the modifier states of
   * ghostStates, so it takes no damage and is not targeted; off restores
   * both. The hero's collision with other heroes is the game's own.
   */
  setGhost(ghost: boolean): void {
    this.ghostNow = ghost
    const pawn = this.player.pawn()
    if (pawn) this.syncLook(pawn.entity)
  }

  /**
   * syncLook writes the ghost look and states to the hero entity when the
   * entity or the ghost setting differs from what it last wrote, so a hero
   * that respawned takes them again.
   */
  private syncLook(entity: number): void {
    // Skip a hero that already wears the look.
    if (this.looked?.entity === entity && this.looked.ghost === this.ghostNow) {
      return
    }
    this.looked = { entity, ghost: this.ghostNow }
    for (const state of ghostStates) {
      holdModifierState(entity, state, this.ghostNow)
    }
    const alpha = this.ghostNow ? ghostAlpha : 255
    const mode = this.ghostNow ? renderTransColor : 0
    writeField(entity, 'CBaseModelEntity', 'm_nRenderMode', 'uint8', mode)
    writeField(
      entity,
      'CBaseModelEntity',
      'm_clrRender',
      'uint32',
      0xffffff | (alpha << 24),
    )
  }

  /** stop ends the puppet and removes its bot; a hero that is not a bot stays. */
  stop(): void {
    // Stop moving, and draw the hero solid again.
    live.delete(this)
    const pawn = this.player.pawn()
    if (pawn && this.ghostNow) {
      this.ghostNow = false
      this.syncLook(pawn.entity)
    }
    const slot = this.player.slot
    if (players().some((c) => c.player.slot === slot && c.bot)) {
      this.player.removeBot()
    }
  }

  /**
   * advance moves playback to the game clock at seconds and places the hero.
   * The library calls it each frame; a mod calls it only to step a puppet by
   * hand.
   */
  advance(seconds: number): void {
    // Playback moves by the game time since the last frame.
    const elapsed = Math.min(seconds - (this.clock ?? seconds), longestFrame)
    this.clock = seconds
    if (this.current === 'playing' && elapsed > 0) {
      this.tickAt += elapsed * ticksPerSecond * this.rateNow
      const end = trackTicks(this.track)
      if (this.tickAt >= end || this.tickAt <= 0) {
        this.tickAt = Math.min(Math.max(this.tickAt, 0), end)
        this.current = 'ended'
      }
      this.drawn = false
    }

    // A paused hero is placed once per seek, a playing one every frame.
    if (!this.drawn) this.place()
  }

  /** place puts the hero at the current tick; it retries next frame without a hero. */
  private place(): void {
    // The hero must exist to be placed.
    const pawn = this.player.pawn()
    const pose = poseAt(this.track, this.tickAt)
    if (!pawn || !pose) return
    this.syncLook(pawn.entity)

    // A large gap teleports; otherwise the hero glides with the recorded motion.
    const gap = Math.hypot(
      pose.position.x - pawn.position.x,
      pose.position.y - pawn.position.y,
      pose.position.z - pawn.position.z,
    )
    const placed =
      gap > this.snapUnits || this.current !== 'playing'
        ? this.player.teleport(pose.position, pose.facing)
        : this.player.move(pose.position, pose.facing, pose.velocity)
    if (placed) this.drawn = true
    this.setHealth(pawn.entity, pose.health)
  }

  /**
   * setHealth gives the hero the recorded health. A recorded zero kills the
   * hero once; the first health above zero after that respawns it, and the
   * next frame places it.
   */
  private setHealth(entity: number, health: number | undefined): void {
    // A recorded death or revival changes whether the hero lives.
    if (health === undefined) return
    if (health <= 0) {
      if (!this.dead) this.dead = this.player.kill()
      return
    }
    if (this.dead) {
      this.dead = !this.player.respawn()
      this.drawn = false
      return
    }
    const rounded = Math.round(health)
    if (rounded !== this.lastHealth) {
      writeField(entity, 'CBaseEntity', 'm_iHealth', 'int32', rounded)
      this.lastHealth = rounded
    }
  }
}
