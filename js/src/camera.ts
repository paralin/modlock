// The hideout's orbit camera on any map. The pawn's networked
// m_bInHideoutZone moves its client to the hideout view: the orbit camera at
// the game's entry level, with the hideout HUD in place of the game HUD. The
// wheel zooms only while the pawn also holds MODIFIER_STATE_HIDEOUT_ZONE, since
// the server ignores the wheel's orbit_camera_in and orbit_camera_out without
// it. The wheel's first zoom adds modifier_citadel_orbit_camera, which keeps
// the zoom level, so returning the normal camera removes it too.
//
// playCamera plays curves (curve.ts) on moveCamera's queued lerps: the
// operations on one param in one context run in order, so a whole curve goes
// to the client at once and plays without the mod's timers.

import {
  type CameraCurve,
  chain,
  durationOf,
  type Ease,
  ease,
  hold,
  type LerpShape,
  line,
  reverse,
  wave,
} from './curve.js'
import {
  type CameraOptions,
  type CameraParam,
  type CameraValue,
  fireInput,
  holdModifierState,
  type Player,
  writeField,
} from './host.gen.js'
import { every, type Timer } from './timer.js'

/**
 * setOrbitCamera gives the player's hero the hideout's orbit camera, zoomed
 * with the wheel, or returns the normal camera and drops the wheel's zoom.
 * The hideout HUD replaces the game HUD while it is on. It returns false when
 * the player has no hero or a call fails.
 */
export function setOrbitCamera(player: Player, enabled: boolean): boolean {
  // A player between lives has no hero to switch.
  const pawn = player.pawn()
  if (!pawn?.entity) return false

  // The flag picks the view; the held state lets the wheel zoom it.
  const flag = writeField(
    pawn.entity,
    'CCitadelPlayerPawn',
    'm_bInHideoutZone',
    'bool',
    enabled,
  )
  const zoom = holdModifierState(
    pawn.entity,
    'MODIFIER_STATE_HIDEOUT_ZONE',
    enabled,
  )
  if (enabled) return flag && zoom

  // The wheel's orbit modifier keeps its zoom level after the flag clears.
  const orbit = fireInput(pawn.entity, 'RemoveModifier', {
    text: 'modifier_citadel_orbit_camera',
  })
  return flag && zoom && orbit
}

/** CameraPlayOptions plays curves on a player's camera. */
export interface CameraPlayOptions {
  /**
   * tracks holds a curve for each param that moves. All tracks start
   * together, so a pull back and a wider view can share one timeline.
   */
  tracks: Partial<Record<CameraParam, CameraCurve>>
  /** relative adds the curves' values to each param's usual value. */
  relative?: boolean
  /** delay starts the tracks after this many seconds. */
  delay?: number
  /** loop plays the tracks again each time they end, until cancelled. */
  loop?: boolean
  /**
   * rate is how many lerps each second play a span the game's lerp cannot
   * draw; omitted, 30. A straight or bias and gain span is always one lerp.
   */
  rate?: number
  /** context names the operations; omitted, a name of its own. */
  context?: string
  /** priority decides between these tracks and another context's moves. */
  priority?: number
  /** target is the entity handle of the hero whose camera moves. */
  target?: number
}

/** CameraMove is a set of curves playing on a player's camera. */
export interface CameraMove {
  /** context names the operations. */
  readonly context: string
  /** duration is how long the move runs, in seconds; Infinity when it loops. */
  readonly duration: number
  /** cancel ends the move at once and returns each param to its usual value. */
  cancel(): boolean
}

/** moves counts the moves started, to give each its own context. */
let moves = 0

/**
 * playCamera plays a curve for each param of the player's camera, sent at
 * once as lerps queued in one context. Each param returns to its usual value
 * after its curve ends, so a curve ends where the camera rests: at zero when
 * relative. Moves in their own contexts take turns by priority. It returns
 * undefined when the player has no hero or a call fails.
 */
export function playCamera(
  player: Player,
  options: CameraPlayOptions,
): CameraMove | undefined {
  // Each move takes a context of its own unless the mod names one.
  const context = options.context ?? `modlock.camera.${++moves}`
  const tracks = Object.entries(options.tracks) as [CameraParam, CameraCurve][]
  const length = Math.max(0, ...tracks.map(([, curve]) => durationOf(curve)))
  const send = (delay: number | undefined) =>
    tracks.every(([param, curve]) =>
      sendCurve(player, { ...options, context, param, delay }, curve),
    )

  // A looping move queues two rounds, then one more as each ends, so the
  // queue never runs dry; it stops once the player has no hero.
  let timer: Timer | undefined
  const stop = () => {
    timer?.cancel()
    return player.clearCamera(context)
  }
  let sent = send(options.delay)
  if (sent && options.loop && length > 0) {
    sent = send(undefined)
    timer = every(length, () => {
      if (!send(undefined)) stop()
    })
  }

  // A move that half reached the camera is taken back off it.
  if (!sent) {
    stop()
    return undefined
  }
  return {
    context,
    duration: options.loop ? Infinity : (options.delay ?? 0) + length,
    cancel: stop,
  }
}

/** CameraPulseOptions moves a param away, holds it and brings it back. */
export interface CameraPulseOptions {
  /** param is the camera value the pulse moves. */
  param: CameraParam
  /** by is how far the param moves from its usual value. */
  by: CameraValue
  /** in is how long the move out takes, in seconds. */
  in: number
  /** hold is how long the param stays out, in seconds. */
  hold: number
  /** out is how long the move back takes, in seconds. */
  out: number
  /** ease shapes the move out; the move back is its mirror. Omitted, linear. */
  ease?: Ease
  /** context names the pulse's operations; omitted, a name of its own. */
  context?: string
  /** priority decides between this pulse and another context's moves. */
  priority?: number
  /** target is the entity handle of the hero whose camera moves. */
  target?: number
}

/**
 * cameraPulse moves one param of the player's camera by an amount, holds it
 * there and brings it back, as an ability pulls the camera back while it
 * charges. It returns undefined when the player has no hero or a call fails.
 */
export function cameraPulse(
  player: Player,
  options: CameraPulseOptions,
): CameraMove | undefined {
  const { param, by, ease: shape = ease.linear } = options
  const rest = zero(by)
  const curve = chain(
    line(rest, by, options.in, shape),
    hold(by, options.hold),
    reverse(line(rest, by, options.out, shape)),
  )
  return playCamera(player, {
    ...options,
    relative: true,
    tracks: { [param]: curve },
  })
}

/** CameraShakeOptions shakes a param around its usual value. */
export interface CameraShakeOptions {
  /** param is the camera value that shakes; omitted, horizontalOffset. */
  param?: CameraParam
  /** amplitude is how far the first swing reaches. */
  amplitude: number
  /** duration is how long the shake runs, in seconds. */
  duration: number
  /** frequency is how many full swings it makes each second; omitted, 8. */
  frequency?: number
  /** context names the shake's operations; omitted, a name of its own. */
  context?: string
  /** priority decides between this shake and another context's moves. */
  priority?: number
  /** target is the entity handle of the hero whose camera moves. */
  target?: number
}

/**
 * cameraShake swings one param of the player's camera to either side of its
 * usual value, smaller each time, and settles back. It returns undefined
 * when the player has no hero or a call fails.
 */
export function cameraShake(
  player: Player,
  options: CameraShakeOptions,
): CameraMove | undefined {
  const { amplitude, duration } = options
  const curve = wave({
    amplitude,
    duration,
    frequency: options.frequency ?? 8,
    decay: true,
  })
  const param = options.param ?? 'horizontalOffset'
  return playCamera(player, {
    ...options,
    relative: true,
    tracks: { [param]: curve },
  })
}

/** SendOptions names where one param's curve goes. */
interface SendOptions extends CameraPlayOptions {
  context: string
  param: CameraParam
}

/**
 * sendCurve queues one param's curve as lerps: one for a span with a lerp
 * shape, and for any other span one per sample at the rate. Only the first lerp waits
 * for the delay; the rest queue behind it.
 */
function sendCurve(
  player: Player,
  options: SendOptions,
  curve: CameraCurve,
): boolean {
  // Every lerp of the track shares its context; only the first carries the
  // delay.
  const { context, param, relative, priority, target } = options
  const rate = options.rate ?? 30
  let delay = options.delay
  const lerp = (
    start: CameraValue,
    end: CameraValue,
    duration: number,
    shape: LerpShape = {},
  ) => {
    const sent = player.moveCamera({
      context,
      param,
      motion: { lerp: { start, end, duration, ...shape } },
      delay,
      relative,
      priority,
      target,
    } satisfies CameraOptions)
    delay = undefined
    return sent
  }

  // A span the game draws goes out as one lerp, any other as samples.
  return curve.every((span) => {
    // An empty span adds nothing to the queue.
    if (span.duration <= 0) return true
    if (span.lerp) {
      return lerp(span.at(0), span.at(1), span.duration, span.lerp)
    }

    // Samples split the span evenly, at least one per span.
    const steps = Math.max(1, Math.ceil(span.duration * rate))
    for (let i = 0; i < steps; i++) {
      const sent = lerp(
        span.at(i / steps),
        span.at((i + 1) / steps),
        span.duration / steps,
      )
      if (!sent) return false
    }
    return true
  })
}

/** zero returns the zero of a camera value's kind. */
function zero(value: CameraValue): CameraValue {
  return typeof value === 'number' ? 0 : { x: 0, y: 0, z: 0 }
}
