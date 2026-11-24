// flight integrates an acro-mode quadcopter: the sticks set body rotation
// rates, thrust pushes along the body's up axis, and gravity and drag act on
// the velocity. Nothing holds the drone level, so it rolls and loops freely.
import type { Angles, Vector } from 'modlock'

/** Sticks is one frame of pilot input. */
export interface Sticks {
  /** throttle runs from 0 (motors idle) to 1 (full thrust). */
  throttle: number
  /**
   * roll, pitch and yaw run from -1 to 1: roll right, nose down and nose
   * right are positive.
   */
  roll: number
  pitch: number
  yaw: number
}

/** idle is the input that holds the attitude with the motors cut. */
export const idle: Sticks = { throttle: 0, roll: 0, pitch: 0, yaw: 0 }

/**
 * Drone is the drone's pose and motion in world units (inches). forward,
 * left and up are its body axes in world coordinates, kept orthonormal, so
 * any attitude, upside down included, has one representation.
 */
export interface Drone {
  position: Vector
  velocity: Vector
  forward: Vector
  left: Vector
  up: Vector
}

/** gravity is Source's 386 inches per second squared, one g. */
const gravity = 386

/** thrustToWeight is full throttle's acceleration in g, a racing quad's. */
const thrustToWeight = 4

/** drag slows the drone in proportion to its speed, per second. */
const drag = 0.6

/** maxRate is a full stick's rotation rate in degrees per second. */
const maxRate = 670

/** expo softens the sticks around center: 0 is linear, 1 is cubic. */
const expo = 0.4

/** launch places a level drone at position, its nose at yaw degrees. */
export function launch(position: Vector, yaw: number): Drone {
  const r = (yaw * Math.PI) / 180
  return {
    position,
    velocity: { x: 0, y: 0, z: 0 },
    forward: { x: Math.cos(r), y: Math.sin(r), z: 0 },
    left: { x: -Math.sin(r), y: Math.cos(r), z: 0 },
    up: { x: 0, y: 0, z: 1 },
  }
}

/** rate maps a stick from -1 to 1 to a rotation rate in radians per second. */
function rate(stick: number): number {
  const s = Math.max(-1, Math.min(1, stick))
  return ((s * (1 - expo) + s * s * s * expo) * maxRate * Math.PI) / 180
}

/**
 * step advances drone by dt seconds under sticks, and returns the position it
 * moves to; the caller checks that path for collisions before keeping it.
 */
export function step(drone: Drone, sticks: Sticks, dt: number): Vector {
  // Turn the body. Source's axes are right-handed (x forward, y left, z up),
  // so roll right turns about +forward, nose down about +left and nose right
  // about -up.
  const { forward: f, left: l, up: u } = drone
  const spin = add(
    add(scale(f, rate(sticks.roll)), scale(l, rate(sticks.pitch))),
    scale(u, -rate(sticks.yaw)),
  )
  const speed = length(spin)
  if (speed > 0) {
    const axis = scale(spin, 1 / speed)
    const angle = speed * dt
    drone.forward = rotate(f, axis, angle)
    drone.up = rotate(u, axis, angle)
  }

  // Keep the axes orthonormal against rounding.
  drone.forward = normalize(drone.forward)
  drone.up = normalize(
    sub(drone.up, scale(drone.forward, dot(drone.forward, drone.up))),
  )
  drone.left = cross(drone.up, drone.forward)

  // Thrust along up, gravity down, drag against the motion.
  const throttle = Math.max(0, Math.min(1, sticks.throttle))
  const accel = add(
    scale(drone.up, throttle * thrustToWeight * gravity),
    add({ x: 0, y: 0, z: -gravity }, scale(drone.velocity, -drag)),
  )
  drone.velocity = add(drone.velocity, scale(accel, dt))
  return add(drone.position, scale(drone.velocity, dt))
}

/**
 * angles converts the drone's body axes to Source's pitch, yaw and roll in
 * degrees, as MatrixAngles does: looking straight up or down, yaw takes the
 * left axis and roll is zero.
 */
export function angles(drone: Drone): Angles {
  // Read pitch and yaw from the nose, and roll from how far left has risen.
  const { forward: f, left: l, up: u } = drone
  const flat = Math.hypot(f.x, f.y)
  const deg = 180 / Math.PI
  if (flat > 0.001) {
    return {
      pitch: Math.atan2(-f.z, flat) * deg,
      yaw: Math.atan2(f.y, f.x) * deg,
      roll: Math.atan2(l.z, u.z) * deg,
    }
  }
  return {
    pitch: Math.atan2(-f.z, flat) * deg,
    yaw: Math.atan2(-l.x, l.y) * deg,
    roll: 0,
  }
}

/** rotate turns v by angle radians about the unit axis (Rodrigues). */
function rotate(v: Vector, axis: Vector, angle: number): Vector {
  const c = Math.cos(angle)
  const s = Math.sin(angle)
  return add(
    add(scale(v, c), scale(cross(axis, v), s)),
    scale(axis, dot(axis, v) * (1 - c)),
  )
}

function add(a: Vector, b: Vector): Vector {
  return { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z }
}

function sub(a: Vector, b: Vector): Vector {
  return { x: a.x - b.x, y: a.y - b.y, z: a.z - b.z }
}

function scale(v: Vector, k: number): Vector {
  return { x: v.x * k, y: v.y * k, z: v.z * k }
}

function dot(a: Vector, b: Vector): number {
  return a.x * b.x + a.y * b.y + a.z * b.z
}

function cross(a: Vector, b: Vector): Vector {
  return {
    x: a.y * b.z - a.z * b.y,
    y: a.z * b.x - a.x * b.z,
    z: a.x * b.y - a.y * b.x,
  }
}

function length(v: Vector): number {
  return Math.sqrt(dot(v, v))
}

function normalize(v: Vector): Vector {
  return scale(v, 1 / length(v))
}
