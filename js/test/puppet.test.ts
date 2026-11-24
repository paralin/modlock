import { expect, test } from 'bun:test'

import { poseAt, trackTicks, type Track } from '../src/kit/puppet.js'

test('poseAt blends samples, turns the short way and clamps the ends', () => {
  // Two samples one second apart, the facing crossing the 360 degree seam.
  const track: Track = {
    stepTicks: 64,
    x: [0, 100],
    y: [0, 0],
    z: [10, 10],
    yaw: [350, 10],
    health: [100, 50],
  }
  expect(trackTicks(track)).toBe(64)

  // Halfway is the midpoint, and the velocity is the segment's slope.
  const mid = poseAt(track, 32)!
  expect(mid.position.x).toBe(50)
  expect(mid.facing.yaw).toBe(360)
  expect(mid.health).toBe(75)
  expect(mid.velocity.x).toBe(100)

  // A tick past either end reads that end.
  expect(poseAt(track, 500)!.position.x).toBe(100)
  expect(poseAt(track, -5)!.position.x).toBe(0)
  expect(poseAt({ ...track, x: [], y: [], z: [], yaw: [] }, 0)).toBeUndefined()
})
