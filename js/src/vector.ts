// Vector math over the game's world: points and directions in world units
// (about an inch each), with z up, and view angles in degrees as the game
// gives them: pitch positive looking down, yaw counterclockwise from +x seen
// from above, and roll about the view.
//
// Every function is pure and returns a new value. Each costs a few
// arithmetic operations and one small allocation, so a frame can call them
// freely; a loop over many points that keeps one result can reuse it.

import type { Angles, Vector } from './host.gen.js'

/** at builds a point or direction from its coordinates. */
export function at(x: number, y: number, z: number): Vector {
  return { x, y, z }
}

/** add is a moved by b. */
export function add(a: Vector, b: Vector): Vector {
  return { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z }
}

/** sub is the step from b to a. */
export function sub(a: Vector, b: Vector): Vector {
  return { x: a.x - b.x, y: a.y - b.y, z: a.z - b.z }
}

/** scale is a times s. */
export function scale(a: Vector, s: number): Vector {
  return { x: a.x * s, y: a.y * s, z: a.z * s }
}

/** dot is the dot product of a and b. */
export function dot(a: Vector, b: Vector): number {
  return a.x * b.x + a.y * b.y + a.z * b.z
}

/** length is a's length. */
export function length(a: Vector): number {
  return Math.hypot(a.x, a.y, a.z)
}

/** normalize is a at unit length, or +x for a zero vector. */
export function normalize(a: Vector): Vector {
  const size = length(a)
  return size > 0 ? scale(a, 1 / size) : { x: 1, y: 0, z: 0 }
}

/** lerp is the point t of the way from a to b: a at 0, b at 1. */
export function lerp(a: Vector, b: Vector, t: number): Vector {
  return {
    x: a.x + (b.x - a.x) * t,
    y: a.y + (b.y - a.y) * t,
    z: a.z + (b.z - a.z) * t,
  }
}

/** distance is how far apart a and b are. */
export function distance(a: Vector, b: Vector): number {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z)
}

/** flatDistance is how far apart a and b are seen from above, ignoring height. */
export function flatDistance(a: Vector, b: Vector): number {
  return Math.hypot(a.x - b.x, a.y - b.y)
}

/** toward is the unit direction from from to to, or zero when they meet. */
export function toward(from: Vector, to: Vector): Vector {
  const span = distance(from, to)
  return span > 0 ? scale(sub(to, from), 1 / span) : { x: 0, y: 0, z: 0 }
}

/** yawToward is the yaw that faces from from toward to, in degrees. */
export function yawToward(from: Vector, to: Vector): number {
  return (Math.atan2(to.y - from.y, to.x - from.x) * 180) / Math.PI
}

/** level is the view that looks along yaw, straight ahead. */
export function level(yaw: number): Angles {
  return { pitch: 0, yaw, roll: 0 }
}

/** aimAt is the view from from that looks at to, without roll. */
export function aimAt(from: Vector, to: Vector): Angles {
  const across = flatDistance(from, to)
  return {
    pitch: (-Math.atan2(to.z - from.z, across) * 180) / Math.PI,
    yaw: yawToward(from, to),
    roll: 0,
  }
}

/** forward is the unit direction a view looks along; roll does not change it. */
export function forward(angles: Angles): Vector {
  const pitch = (angles.pitch * Math.PI) / 180
  const yaw = (angles.yaw * Math.PI) / 180
  return {
    x: Math.cos(pitch) * Math.cos(yaw),
    y: Math.cos(pitch) * Math.sin(yaw),
    z: -Math.sin(pitch),
  }
}
