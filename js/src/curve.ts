// Camera curves: values over time for playCamera to send to a player's camera.
// A curve is a list of spans played one after another. A span the game's lerp
// can draw, straight or shaped by its bias and gain, plays as one lerp; any
// other span is sampled into short lerps. Every constructor and combinator here builds spans, so they
// compose: keyframes through points, smooth curves, speeds, orbits, waves,
// chains, repeats and reversals.

import type { CameraValue, Vector } from './host.gen.js'

/**
 * LerpShape is the shape the game's own lerp gives a move: Valve's bias and
 * gain curves, each linear at 0.5. An omitted value is linear.
 */
export interface LerpShape {
  /** bias below 0.5 starts slow and speeds up; above, starts fast. */
  readonly bias?: number
  /** gain above 0.5 starts and ends slow; below, fast at both ends. */
  readonly gain?: number
}

/**
 * Ease maps the fraction of a span played, 0 to 1, to the fraction moved. An
 * ease with a lerp shape is one the game draws itself, so a move with it
 * plays as one lerp. It is a record rather than a function with a property,
 * which scriptc cannot read.
 */
export interface Ease {
  /** at returns the fraction moved a fraction t of the way through. */
  readonly at: (t: number) => number
  /** lerp is the shape the game draws the ease with, if it can. */
  readonly lerp?: LerpShape
}

/** ease holds the common easing curves. */
export const ease = {
  /** linear moves at an even pace. */
  linear: shaped((t) => t, {}),
  /** bias is the game's bias curve; see LerpShape. */
  bias: (b: number): Ease => shaped((t) => bias(t, b), { bias: b }),
  /** gain is the game's gain curve; see LerpShape. */
  gain: (g: number): Ease => shaped((t) => gain(t, g), { gain: g }),
  /** in starts slow and speeds up. */
  in: shaped((t) => t * t * t),
  /** out starts fast and slows to a stop. */
  out: shaped((t) => 1 - (1 - t) ** 3),
  /** inOut starts and ends slow. */
  inOut: shaped((t) => (t < 0.5 ? 4 * t * t * t : 1 - (2 - 2 * t) ** 3 / 2)),
  /** sine starts and ends slow, more gently than inOut. */
  sine: shaped((t) => (1 - Math.cos(Math.PI * t)) / 2),
  /** back passes its end a little and settles back onto it. */
  back: shaped((t) => 1 + 2.70158 * (t - 1) ** 3 + 1.70158 * (t - 1) ** 2),
  /** elastic overshoots its end and rings down onto it. */
  elastic: shaped((t) =>
    t <= 0 || t >= 1
      ? t
      : 2 ** (-10 * t) * Math.sin(((t * 10 - 0.75) * 2 * Math.PI) / 3) + 1,
  ),
  /** bounce drops onto its end and bounces to rest. */
  bounce: shaped((t) => {
    // Four parabolas, each a smaller bounce than the one before.
    const k = 7.5625
    if (t < 1 / 2.75) return k * t * t
    if (t < 2 / 2.75) return k * (t - 1.5 / 2.75) ** 2 + 0.75
    if (t < 2.5 / 2.75) return k * (t - 2.25 / 2.75) ** 2 + 0.9375
    return k * (t - 2.625 / 2.75) ** 2 + 0.984375
  }),
} as const

/**
 * shaped is the ease that moves by at, drawn by the game with lerp when it has
 * one. It declares its result, so every ease has the one layout scriptc
 * gives Ease.
 */
export function shaped(at: (t: number) => number, lerp?: LerpShape): Ease {
  return { at, lerp }
}

/** CameraSpan is one piece of a curve. */
export interface CameraSpan {
  /** duration is how long the span plays, in seconds. */
  readonly duration: number
  /** at returns the value a fraction u of the way through, 0 to 1. */
  readonly at: (u: number) => CameraValue
  /**
   * lerp marks a span the game plays as one lerp from at(0) to at(1) with this
   * shape; a span without it is sampled.
   */
  readonly lerp?: LerpShape
}

/** CameraCurve is a list of spans played in order. */
export type CameraCurve = readonly CameraSpan[]

/** CameraKey is one point of a keyframed curve. */
export interface CameraKey {
  /** at is the time of the key, in seconds after the curve starts. */
  at: number
  /** value is the param's value at the key. */
  value: CameraValue
  /** ease shapes the move from the key before this one; omitted, linear. */
  ease?: Ease
}

/** KeyframeOptions shapes the curve between keys. */
export interface KeyframeOptions {
  /**
   * smooth passes through the keys on a curve, without corners, instead of a
   * straight line from each to the next.
   */
  smooth?: boolean
}

/** line moves from one value to another over a duration. */
export function line(
  from: CameraValue,
  to: CameraValue,
  duration: number,
  shape: Ease = ease.linear,
): CameraCurve {
  return [{ duration, lerp: shape.lerp, at: (u) => mix(from, to, shape.at(u)) }]
}

/** hold keeps a value for a duration. */
export function hold(value: CameraValue, duration: number): CameraCurve {
  return [{ duration, lerp: {}, at: () => value }]
}

/**
 * keyframes passes through keys in time order. A key's ease shapes the move
 * into it; two keys at one time make a jump, and two with one value a hold.
 * The curve starts at the first key, so a first key after zero waits there.
 */
export function keyframes(
  keys: readonly CameraKey[],
  options: KeyframeOptions = {},
): CameraCurve {
  // A first key after zero holds its value until its time.
  const spans: CameraSpan[] = []
  const [first] = keys
  if (first && first.at > 0) spans.push(...hold(first.value, first.at))
  keys.forEach((to, i) => {
    // Each pair of keys is one span; a jump takes no time and adds none.
    const from = keys[i - 1]
    if (!from || to.at <= from.at) return
    const duration = to.at - from.at
    const shape = to.ease ?? ease.linear
    if (!options.smooth) {
      spans.push(...line(from.value, to.value, duration, shape))
      return
    }

    // A smooth span bends toward the keys on either side, on a Catmull-Rom
    // spline, which passes through every key.
    const before = keys[i - 2]?.value ?? from.value
    const after = keys[i + 1]?.value ?? to.value
    spans.push({
      duration,
      at: (u) => spline(before, from.value, to.value, after, shape.at(u)),
    })
  })
  return spans
}

/** ThroughOptions times a curve through points by its speed. */
export interface ThroughOptions extends KeyframeOptions {
  /** speed is how far the param moves each second. */
  speed: number
  /** ease shapes each move between two points; omitted, linear. */
  ease?: Ease
}

/**
 * through moves through points at a speed, so each move takes as long as its
 * distance needs.
 */
export function through(
  points: readonly CameraValue[],
  options: ThroughOptions,
): CameraCurve {
  let at = 0
  let last = points[0]
  const keys = points.map((value) => {
    if (last !== undefined) at += distance(last, value) / options.speed
    last = value
    return { at, value, ease: options.ease }
  })
  return keyframes(keys, options)
}

/** OrbitOptions circles a point. */
export interface OrbitOptions {
  /** center is the point the circle goes around. */
  center: Vector
  /** radius is the circle's radius. */
  radius: number
  /** duration is how long the orbit takes, in seconds. */
  duration: number
  /** turns is how many times it goes around; omitted, 1. Negative turns clockwise. */
  turns?: number
  /** start is the angle it starts at, in degrees from the x axis; omitted, 0. */
  start?: number
  /** rise is how far it climbs over the whole orbit; omitted, 0. */
  rise?: number
  /** ease shapes the pace around the circle; omitted, linear. */
  ease?: Ease
}

/** orbit circles a point, for the target param, as a spiral when it rises. */
export function orbit(options: OrbitOptions): CameraCurve {
  // Angles run in radians from the start angle, around the z axis.
  const { center, radius, duration } = options
  const turns = options.turns ?? 1
  const start = ((options.start ?? 0) * Math.PI) / 180
  const rise = options.rise ?? 0
  const shape = options.ease ?? ease.linear
  return [
    {
      duration,
      at: (u) => {
        const t = shape.at(u)
        const angle = start + t * turns * 2 * Math.PI
        return {
          x: center.x + radius * Math.cos(angle),
          y: center.y + radius * Math.sin(angle),
          z: center.z + rise * t,
        }
      },
    },
  ]
}

/** WaveOptions swings a number around zero. */
export interface WaveOptions {
  /** amplitude is how far the first swing reaches. */
  amplitude: number
  /** frequency is how many full swings it makes each second. */
  frequency: number
  /** duration is how long it swings, in seconds. */
  duration: number
  /**
   * decay shrinks the swings to nothing by the end when true, as a shake
   * settles; omitted, they keep their size.
   */
  decay?: boolean
}

/** wave swings a number to either side of zero and ends at zero. */
export function wave(options: WaveOptions): CameraCurve {
  const { amplitude, frequency, duration, decay } = options
  return [
    {
      duration,
      at: (u) =>
        amplitude *
        (decay ? 1 - u : 1) *
        Math.sin(2 * Math.PI * frequency * duration * u),
    },
  ]
}

/** chain plays curves one after another. */
export function chain(...curves: readonly CameraCurve[]): CameraCurve {
  return curves.flat()
}

/**
 * reverse plays a curve backward. A gain curve is its own mirror and stays one
 * lerp; a biased span backward is no bias curve, so it is sampled.
 */
export function reverse(curve: CameraCurve): CameraCurve {
  return curve
    .map((span) => ({
      duration: span.duration,
      lerp: span.lerp?.bias === undefined ? span.lerp : undefined,
      at: (u: number) => span.at(1 - u),
    }))
    .reverse()
}

/** repeat plays a curve a number of times. */
export function repeat(curve: CameraCurve, times: number): CameraCurve {
  return Array.from({ length: Math.max(0, times) }, () => curve).flat()
}

/** pingPong plays a curve forward and then backward. */
export function pingPong(curve: CameraCurve): CameraCurve {
  return chain(curve, reverse(curve))
}

/** durationOf returns how long a curve plays, in seconds. */
export function durationOf(curve: CameraCurve): number {
  return curve.reduce((sum, span) => sum + Math.max(0, span.duration), 0)
}

/**
 * valueAt returns a curve's value a number of seconds in, or its last value
 * once it has ended. An empty curve has no value.
 */
export function valueAt(
  curve: CameraCurve,
  seconds: number,
): CameraValue | undefined {
  let left = seconds
  for (const span of curve) {
    if (span.duration <= 0) continue
    if (left <= span.duration) return span.at(Math.max(0, left) / span.duration)
    left -= span.duration
  }
  return curve.at(-1)?.at(1)
}

/** bias is Valve's Bias: t raised to the power that maps 0.5 to b. */
function bias(t: number, b: number): number {
  return t ** (Math.log(b) / Math.log(0.5))
}

/** gain is Valve's Gain: two mirrored bias halves meeting at the middle. */
function gain(t: number, g: number): number {
  return t < 0.5 ? bias(2 * t, 1 - g) / 2 : 1 - bias(2 - 2 * t, 1 - g) / 2
}

/** mix returns the value a fraction t of the way from a to b. */
function mix(a: CameraValue, b: CameraValue, t: number): CameraValue {
  if (typeof a === 'number' && typeof b === 'number') return a + (b - a) * t
  const [p, q] = [vector(a), vector(b)]
  return {
    x: p.x + (q.x - p.x) * t,
    y: p.y + (q.y - p.y) * t,
    z: p.z + (q.z - p.z) * t,
  }
}

/** spline returns the Catmull-Rom point a fraction t from p1 to p2. */
function spline(
  p0: CameraValue,
  p1: CameraValue,
  p2: CameraValue,
  p3: CameraValue,
  t: number,
): CameraValue {
  const f = (a: number, b: number, c: number, d: number) =>
    0.5 *
    (2 * b +
      (c - a) * t +
      (2 * a - 5 * b + 4 * c - d) * t * t +
      (3 * b - a - 3 * c + d) * t * t * t)
  if (
    typeof p0 === 'number' &&
    typeof p1 === 'number' &&
    typeof p2 === 'number' &&
    typeof p3 === 'number'
  ) {
    return f(p0, p1, p2, p3)
  }
  const [a, b, c, d] = [vector(p0), vector(p1), vector(p2), vector(p3)]
  return {
    x: f(a.x, b.x, c.x, d.x),
    y: f(a.y, b.y, c.y, d.y),
    z: f(a.z, b.z, c.z, d.z),
  }
}

/** distance returns how far apart two values are. */
function distance(a: CameraValue, b: CameraValue): number {
  if (typeof a === 'number' && typeof b === 'number') return Math.abs(b - a)
  const [p, q] = [vector(a), vector(b)]
  return Math.hypot(q.x - p.x, q.y - p.y, q.z - p.z)
}

/** vector reads a value as a position; a number lies on the x axis. */
function vector(value: CameraValue): Vector {
  return typeof value === 'number' ? { x: value, y: 0, z: 0 } : value
}
