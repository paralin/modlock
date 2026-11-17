import { expect, spyOn, test } from 'bun:test'

import {
  Call,
  CommandEvent,
  FrameEvent,
  Reply,
  ServiceCall,
  ServiceReply,
} from '../../src/wasm/wasm.pb.js'
import { bind, deliver as dispatch } from '../src/host.js'
import { command, every, onFrame } from '../src/index.js'
import { caller } from '../src/profile.js'

// The library answers every host call it makes with an empty reply.
bind(() => Reply.toBinary({}))

/** deliver sends the mod one host event and returns its decoded reply. */
function deliver(method: string, request: Uint8Array): Reply {
  return Reply.fromBinary(dispatch(Call.toBinary({ method, request })))
}

/** frame delivers a frame at seconds on the game clock. */
function frame(seconds: number): void {
  deliver('Frame', FrameEvent.toBinary({ timeSeconds: seconds }))
}

/** Row is one handler's timings in the profiler's answer. */
interface Row {
  kind: string
  at: string
  calls: number
  ms: number
  max: number
  over: number
}

/** Timings is the profiler's answer. */
interface Timings {
  frames: Row
  sites: Row[]
}

/** profileCall calls the profiler's service with method and decodes its answer. */
function profileCall(method: string): Timings | undefined {
  const reply = deliver(
    'Serve',
    ServiceCall.toBinary({ service: 'modlock.profile', method }),
  )
  expect(reply.error ?? '').toBe('')
  const payload = ServiceReply.fromBinary(
    reply.response ?? new Uint8Array(),
  ).payload
  if (!payload || payload.length === 0) {
    return undefined
  }
  return JSON.parse(new TextDecoder().decode(payload)) as Timings
}

/** registeredAt matches the location of the line of this file that starts with code. */
function registeredAt(code: string): RegExp {
  const line = source.split('\n').findIndex((text) => text.startsWith(code)) + 1
  return new RegExp(`profile\\.test\\.ts:${line}:\\d+$`)
}
const source = await Bun.file(import.meta.path).text()

// The mod's handlers, each registered on its own line of this file.
let frames = 0
let ticks = 0
let commands = 0
onFrame(() => frames++)
every(0.01, () => ticks++)
command('go', () => commands++)

test('without the profiler the library reads no clock', () => {
  const now = spyOn(performance, 'now')
  try {
    for (let i = 1; i <= 5; i++) {
      deliver('Command', CommandEvent.toBinary({ player: 0, line: 'go' }))
      frame(i)
    }
  } finally {
    now.mockRestore()
  }
  expect(frames).toBe(5)
  expect(ticks).toBeGreaterThan(0)
  expect(commands).toBe(5)
  expect(now).toHaveBeenCalledTimes(0)
})

test('the profiler times each handler by where it was registered', () => {
  // The first read turns timing on and reports nothing yet.
  expect(profileCall('read')?.sites).toEqual([])
  for (let i = 10; i <= 13; i++) {
    deliver('Command', CommandEvent.toBinary({ player: 0, line: 'go' }))
    frame(i)
  }

  // The next read has each handler's calls, located at its registration.
  const read = profileCall('read')
  expect(read?.frames.calls).toBe(4)
  const sites = new Map(read?.sites.map((row) => [row.kind, row]))
  expect(sites.get('onFrame')?.calls).toBe(4)
  expect(sites.get('every')?.calls).toBe(4)
  expect(sites.get('command /go')?.calls).toBe(4)
  expect(sites.get('onFrame')?.at).toMatch(
    registeredAt('onFrame(() => frames++)'),
  )
  expect(sites.get('every')?.at).toMatch(registeredAt('every(0.01'))
  expect(sites.get('command /go')?.at).toMatch(registeredAt("command('go'"))

  // A read resets the counts, and stop turns timing off again.
  frame(14)
  expect(profileCall('read')?.frames.calls).toBe(1)
  expect(profileCall('stop')).toBeUndefined()
  const now = spyOn(performance, 'now')
  try {
    frame(15)
  } finally {
    now.mockRestore()
  }
  expect(now).toHaveBeenCalledTimes(0)
})

test('caller reads both QuickJS and JavaScriptCore stacks', () => {
  const quickjs =
    '    at site (mod.js:10:5)\n    at onFrame (mod.js:20:3)\n    at <anonymous> (mod.js:300:12)\n'
  expect(caller(quickjs, 2)).toBe('mod.js:300:12')
  const jsc =
    'site@/a/mod.js:1:2\nonFrame@/a/mod.js:3:4\nmodule code@/a/mod.js:5:6'
  expect(caller(jsc, 2)).toBe('/a/mod.js:5:6')
  expect(caller('', 2)).toBe('')
})
