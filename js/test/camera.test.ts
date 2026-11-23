import { expect, test } from 'bun:test'

import {
  HoldEntityModifierRequest,
  HoldModifierRequest,
} from '../../src/wasm/abilities/abilities.pb.js'
import {
  AliveResponse,
  EntityOptions,
  EntityRequest,
  EntityResponse,
  FireInputRequest,
  MoveEntityRequest,
  ShowEntityOnlyToRequest,
} from '../../src/wasm/entities/entities.pb.js'
import { WriteFieldRequest } from '../../src/wasm/fields/fields.pb.js'
import {
  CameraOptions,
  PawnResponse,
} from '../../src/wasm/players/players.pb.js'
import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import {
  cameraPulse,
  cameraShake,
  entityCamera,
  playCamera,
} from '../src/camera.js'
import * as curve from '../src/curve.js'
import { Player } from '../src/host.gen.js'
import { bind } from '../src/host.js'
import { advance } from '../src/timer.js'

/** fail names the calls, comma separated, the host answers with an error. */
let fail = ''
/** nextEntity issues distinct handles for CreateEntity calls in tests. */
let nextEntity = 100
/** pawnEntity is the hero pawn handle Pawn returns; 0 simulates no hero. */
let pawnEntity = 42

/** sent holds the calls the host received. */
const sent: {
  method: string
  request: Uint8Array
  options?: CameraOptions
}[] = []
bind((bytes) => {
  const { method = '', request = new Uint8Array() } = Call.fromBinary(bytes)
  sent.push({
    method,
    request,
    options:
      method === 'MoveCamera' ? CameraOptions.fromBinary(request) : undefined,
  })
  if (fail.split(',').includes(method))
    return Reply.toBinary({ error: 'refused' })
  if (method === 'Pawn') {
    return Reply.toBinary({
      response: PawnResponse.toBinary(
        pawnEntity ? { pawn: { entity: pawnEntity } } : {},
      ),
    })
  }
  if (method === 'CreateEntity') {
    return Reply.toBinary({
      response: EntityResponse.toBinary({ entity: nextEntity++ }),
    })
  }
  if (method === 'FireInput' || method === 'RemoveEntity') {
    return Reply.toBinary({
      response: AliveResponse.toBinary({ alive: true }),
    })
  }
  return Reply.toBinary({})
})

/** lerps returns each sent lerp of a number as its start, end and duration. */
function lerps(): number[][] {
  return sent.flatMap(({ options }) => {
    // Lerps of a position read as NaN here.
    const motion = options?.motion?.motion
    if (motion?.case !== 'lerp') return []
    const { start, end, duration } = motion.value
    const scalar = (v: typeof start) =>
      v?.value?.case === 'number' ? v.value.value : NaN
    return [[scalar(start), scalar(end), duration ?? 0]]
  })
}

/** near reports whether two numbers agree to a thousandth. */
function near(a: number, b: number): boolean {
  return Math.abs(a - b) < 1e-3
}

test('curves compose and play as queued lerps', () => {
  // A straight or bias and gain line is one of the game's lerps, and a
  // reversed bias is not.
  expect(curve.line(0, 10, 2).every((span) => span.lerp)).toBe(true)
  const biased = curve.line(0, 10, 2, curve.ease.bias(0.25))
  expect(biased.every((span) => span.lerp?.bias === 0.25)).toBe(true)
  expect(curve.valueAt(biased, 1)).toBe(2.5)
  expect(curve.reverse(biased).some((span) => span.lerp)).toBe(false)

  // Any other ease is sampled at the rate, and passes through its ends.
  const eased = curve.line(0, 10, 2, curve.ease.out)
  expect(eased.some((span) => span.lerp)).toBe(false)
  expect([0, 2].map((t) => curve.valueAt(eased, t))).toEqual([0, 10])
  expect(curve.valueAt(eased, 1)).toBeGreaterThan(5)

  // Keyframes wait at a late first key, jump where two keys share a time,
  // and a smooth curve still passes through every key.
  const keys = [
    { at: 1, value: 90 },
    { at: 1, value: 60 },
    { at: 2, value: 70 },
    { at: 4, value: 40 },
  ]
  expect(curve.durationOf(curve.keyframes(keys))).toBe(4)
  const smooth = curve.keyframes(keys, { smooth: true })
  for (const { at, value } of keys.slice(2)) {
    expect(near(curve.valueAt(smooth, at) as number, value)).toBe(true)
  }

  // Through times each move by its distance; reverse and pingPong mirror.
  const paced = curve.through([0, 300, 200], { speed: 100 })
  expect(curve.durationOf(paced)).toBe(4)
  expect(curve.valueAt(curve.reverse(paced), 1)).toBe(300)
  expect(curve.valueAt(curve.pingPong(paced), 8)).toBe(0)
  expect(curve.durationOf(curve.repeat(paced, 3))).toBe(12)

  // An orbit keeps its radius and climbs by its rise.
  const center = { x: 100, y: 200, z: 50 }
  const ring = curve.orbit({ center, radius: 40, duration: 2, rise: 10 })
  const quarter = curve.valueAt(ring, 0.5) as {
    x: number
    y: number
    z: number
  }
  expect(near(Math.hypot(quarter.x - 100, quarter.y - 200), 40)).toBe(true)
  expect(quarter.z).toBe(52.5)

  // A pulse sends one lerp out, one hold and one back, all relative and in
  // one context of its own.
  const player = new Player(3)
  sent.length = 0
  const pulse = cameraPulse(player, {
    param: 'distance',
    by: 300,
    in: 0.5,
    hold: 2,
    out: 1,
  })!
  expect(pulse.duration).toBe(3.5)
  expect(lerps()).toEqual([
    [0, 300, 0.5],
    [300, 300, 2],
    [300, 0, 1],
  ])
  expect(sent.every(({ options }) => options?.relative)).toBe(true)
  expect(new Set(sent.map(({ options }) => options?.context))).toEqual(
    new Set([pulse.context]),
  )

  // Two tracks share a timeline, each in order; the delay holds only each
  // track's first lerp, and cancel clears only this move's context.
  sent.length = 0
  const both = playCamera(player, {
    delay: 1,
    rate: 10,
    tracks: {
      distance: curve.line(150, 400, 1),
      fieldOfView: curve.line(90, 70, 1, curve.ease.inOut),
    },
  })!
  expect(both.context).not.toBe(pulse.context)
  expect(sent.length).toBe(11)
  expect(sent.filter(({ options }) => options?.delay === 1).length).toBe(2)
  sent.length = 0
  both.cancel()
  expect(sent.map(({ method }) => method)).toEqual(['ClearCamera'])

  // A bias or gain line goes out as one lerp carrying its shape.
  sent.length = 0
  playCamera(player, {
    tracks: { distance: curve.line(0, 300, 1, curve.ease.gain(0.75)) },
  })
  const [shaped] = sent.map(({ options }) => options?.motion?.motion)
  expect(sent.length).toBe(1)
  expect(shaped?.case === 'lerp' && shaped.value.gain).toBe(0.75)

  // A shake settles at zero.
  sent.length = 0
  cameraShake(player, { amplitude: 20, duration: 0.5 })
  expect(near(lerps().at(-1)?.[1] ?? NaN, 0)).toBe(true)

  // A loop queues two rounds ahead.
  advance(0)
  sent.length = 0
  const loop = playCamera(player, {
    loop: true,
    tracks: { verticalOffset: curve.pingPong(curve.line(0, 50, 1)) },
  })!
  expect(loop.duration).toBe(Infinity)
  expect(lerps().length).toBe(4)

  // Each finished round queues the next until the loop is cancelled.
  advance(2)
  expect(lerps().length).toBe(6)
  loop.cancel()
  advance(4)
  expect(lerps().length).toBe(6)

  // A move the host refuses is cleared and reports nothing.
  fail = 'MoveCamera'
  sent.length = 0
  expect(
    cameraPulse(player, { param: 'distance', by: 1, in: 1, hold: 1, out: 1 }),
  ).toBeUndefined()
  expect(sent.at(-1)?.method).toBe('ClearCamera')
  fail = ''
})

test('entityCamera spawns isolated point_camera with roll and cleans up', () => {
  const player = new Player(5)
  pawnEntity = 77
  nextEntity = 200
  fail = ''
  advance(0)
  sent.length = 0

  const cam = entityCamera(player, {
    position: { x: 10, y: 20, z: 30 },
    facing: { pitch: -15, yaw: 90, roll: 45 },
    fov: 105,
    zNear: 4,
    zFar: 16000,
  })!
  expect(cam).toBeDefined()
  expect(cam.entity).toBe(200)

  // Spawns intro_camera and pre_intro_camera, isolates both to slot 5, and
  // holds modifier_citadel_hideout_intro on the living hero.
  const created = sent
    .filter((c) => c.method === 'CreateEntity')
    .map((c) => EntityOptions.fromBinary(c.request))
  expect(created.length).toBe(2)
  expect(created.map((c) => c.designerName)).toEqual([
    'point_camera',
    'point_camera',
  ])
  expect(created.every((c) => c.transmitAlways)).toBe(true)
  expect(
    created.map(
      (c) =>
        c.keyValues?.find((kv) => kv.key === 'targetname')?.value?.value?.value,
    ),
  ).toEqual(['intro_camera', 'pre_intro_camera'])
  expect(
    created[0]?.keyValues?.find((kv) => kv.key === 'FOV')?.value?.value?.value,
  ).toBe(105)

  const audiences = sent
    .filter((c) => c.method === 'ShowEntityOnlyTo')
    .map((c) => ShowEntityOnlyToRequest.fromBinary(c.request))
  expect(audiences).toEqual([
    { entity: 200, players: [5] },
    { entity: 201, players: [5] },
  ])

  const held = sent
    .filter((c) => c.method === 'HoldModifier')
    .map((c) => HoldModifierRequest.fromBinary(c.request))
  expect(held).toEqual([
    { player: 5, modifier: 'modifier_citadel_hideout_intro', active: true },
  ])

  // move() sends full 3-axis roll and over-the-top pitch to both cameras while
  // pre_intro_camera is still alive, and setFov() updates CPointCamera.m_FOV.
  sent.length = 0
  expect(
    cam.move({ x: 100, y: 200, z: 300 }, { pitch: 120, yaw: -45, roll: 180 }),
  ).toBe(true)
  const moves = sent
    .filter((c) => c.method === 'MoveEntity')
    .map((c) => MoveEntityRequest.fromBinary(c.request))
  expect(moves.length).toBe(2)
  expect(moves[0]?.facing).toEqual({ pitch: 120, yaw: -45, roll: 180 })

  sent.length = 0
  expect(cam.setFov(115)).toBe(true)
  const fovWrites = sent
    .filter((c) => c.method === 'WriteField')
    .map((c) => WriteFieldRequest.fromBinary(c.request))
  expect(fovWrites.length).toBe(2)
  expect(fovWrites[0]?.className).toBe('CPointCamera')
  expect(fovWrites[0]?.field).toBe('m_FOV')

  // pre_intro_camera stays alive and keeps following intro_camera, since the
  // game blends between the two for seconds after the camera opens.
  // The camera reports settled once the game's opening blend has run out.
  expect(cam.settled).toBe(false)
  sent.length = 0
  advance(10)
  expect(cam.settled).toBe(true)
  expect(sent.filter((c) => c.method === 'RemoveEntity')).toEqual([])
  expect(cam.move({ x: 1, y: 2, z: 3 }, { pitch: 0, yaw: 0, roll: -90 })).toBe(
    true,
  )
  expect(
    sent
      .filter((c) => c.method === 'MoveEntity')
      .map((c) => MoveEntityRequest.fromBinary(c.request).entity),
  ).toEqual([200, 201])

  // stop() releases the hold, fires RemoveModifier for both intro modifiers,
  // removes both cameras, and is idempotent.
  sent.length = 0
  expect(cam.stop()).toBe(true)
  expect(
    sent
      .filter((c) => c.method === 'HoldModifier')
      .map((c) => HoldModifierRequest.fromBinary(c.request)),
  ).toEqual([
    { player: 5, modifier: 'modifier_citadel_hideout_intro', active: false },
  ])
  expect(
    sent
      .filter((c) => c.method === 'FireInput')
      .map((c) => FireInputRequest.fromBinary(c.request)),
  ).toEqual([
    {
      entity: 77,
      input: 'RemoveModifier',
      value: {
        value: { case: 'text', value: 'modifier_citadel_hideout_intro' },
      },
    },
    {
      entity: 77,
      input: 'RemoveModifier',
      value: {
        value: { case: 'text', value: 'modifier_citadel_hideout_intro_exit' },
      },
    },
  ])
  expect(
    sent
      .filter((c) => c.method === 'RemoveEntity')
      .map((c) => EntityRequest.fromBinary(c.request).entity),
  ).toEqual([201, 200])

  sent.length = 0
  expect(cam.stop()).toBe(true)
  expect(cam.move({ x: 0, y: 0, z: 0 }, { pitch: 0, yaw: 0, roll: 0 })).toBe(
    false,
  )
  expect(cam.setFov(90)).toBe(false)
  expect(sent.length).toBe(0)

  // instant: false spawns only intro_camera.
  sent.length = 0
  const smooth = entityCamera(player, {
    position: { x: 0, y: 0, z: 0 },
    facing: { pitch: 0, yaw: 0, roll: 0 },
    instant: false,
  })!
  expect(sent.filter((c) => c.method === 'CreateEntity').length).toBe(1)
  smooth.stop()

  // A refused HoldModifier, as on a hero that owns no ability yet, falls back
  // to holding the modifier on the pawn itself, and stop releases that hold.
  fail = 'HoldModifier'
  sent.length = 0
  const fallback = entityCamera(player, {
    position: { x: 0, y: 0, z: 0 },
    facing: { pitch: 0, yaw: 0, roll: 0 },
  })
  expect(fallback).toBeDefined()
  const pawnHolds = () =>
    sent
      .filter((c) => c.method === 'HoldEntityModifier')
      .map((c) => HoldEntityModifierRequest.fromBinary(c.request))
  expect(pawnHolds()).toEqual([
    {
      entity: pawnEntity,
      modifier: 'modifier_citadel_hideout_intro',
      active: true,
    },
  ])
  sent.length = 0
  expect(fallback!.stop()).toBe(true)
  expect(pawnHolds()).toEqual([
    {
      entity: pawnEntity,
      modifier: 'modifier_citadel_hideout_intro',
      active: false,
    },
  ])

  // Refused both ways, it cleans up both spawned cameras.
  fail = 'HoldModifier,HoldEntityModifier'
  sent.length = 0
  expect(
    entityCamera(player, {
      position: { x: 0, y: 0, z: 0 },
      facing: { pitch: 0, yaw: 0, roll: 0 },
    }),
  ).toBeUndefined()
  expect(sent.filter((c) => c.method === 'RemoveEntity').length).toBe(2)
  fail = ''

  // Returns undefined when the player has no hero.
  pawnEntity = 0
  expect(
    entityCamera(player, {
      position: { x: 0, y: 0, z: 0 },
      facing: { pitch: 0, yaw: 0, roll: 0 },
    }),
  ).toBeUndefined()
  pawnEntity = 42
})
