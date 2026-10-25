import { expect, test } from 'bun:test'

import {
  Call,
  CommandEvent,
  CommandResult,
  FrameEvent,
  LogRequest,
  Reply,
  ServerCommandRequest,
} from '../../src/wasm/wasm.pb.js'
import { apply, arena } from '../src/console.js'
import { bind, deliver as dispatch } from '../src/host.js'
import { command, onFrame, ProbeRun } from '../src/index.js'

/** lines holds the log lines and console commands the mod sent the host. */
const lines: string[] = []

/** refuse names a console variable the host refuses to set. */
let refuse = ''

// The library answers every host call it makes with an empty reply, and
// refuses a command that sets refuse.
bind((bytes) => {
  // Record what the mod logged or ran, refusing the one variable.
  const sent = Call.fromBinary(bytes)
  const request = sent.request ?? new Uint8Array()
  if (sent.method === 'Log') {
    lines.push(LogRequest.fromBinary(request).message ?? '')
  }
  if (sent.method === 'ServerCommand') {
    const line = ServerCommandRequest.fromBinary(request).command ?? ''
    lines.push(`> ${line}`)
    if (refuse && line.startsWith(`${refuse} `)) {
      return Reply.toBinary({ error: 'refused' })
    }
  }
  return Reply.toBinary({})
})

/** deliver sends the mod one host event and returns its decoded reply. */
function deliver(method: string, request: Uint8Array): Reply {
  return Reply.fromBinary(dispatch(Call.toBinary({ method, request })))
}

/** frame delivers a frame at seconds on the game clock. */
function frame(seconds: number): void {
  deliver('Frame', FrameEvent.toBinary({ timeSeconds: seconds }))
}

/** send delivers a command line from the player in slot and reports whether the mod claimed it. */
function send(slot: number, line: string): boolean {
  const reply = deliver(
    'Command',
    CommandEvent.toBinary({ player: slot, line }),
  )
  return (
    CommandResult.fromBinary(reply.response ?? new Uint8Array()).claimed ??
    false
  )
}

test('a command is claimed at once and runs with the next frame', () => {
  // Record each command and frame in the order the mod sees them.
  const seen: string[] = []
  command('watch', (player, args) => seen.push(`watch ${player.slot} ${args}`))
  onFrame(() => seen.push('frame'))
  frame(10)
  seen.length = 0

  // The command is claimed but waits for the frame, and runs before its handlers.
  expect(send(3, '  watch  b0fe  ')).toBe(true)
  expect(seen).toEqual([])
  frame(10.016)
  expect(seen).toEqual(['watch 3 b0fe', 'frame'])

  // A command the mod does not know stays with the game.
  expect(send(3, 'unknown')).toBe(false)
})

test('a command that throws does not stop the frame', () => {
  // The failing command logs, and the next one and the frame handlers still run.
  const seen: string[] = []
  command('fail', () => {
    throw new Error('broken')
  })
  command('ok', () => seen.push('ok'))
  frame(20)
  send(1, 'fail')
  send(1, 'ok')
  frame(20.016)
  expect(seen).toEqual(['ok'])
})

test('a mode empties the map with its own settings and probes itself', () => {
  // The arena preset goes first and the mode's own settings after it; a
  // refused variable is named and the rest still apply.
  lines.length = 0
  refuse = 'citadel_koth_enabled'
  const refused = apply({ ...arena, citadel_allow_duplicate_heroes: true })
  refuse = ''
  expect(refused).toEqual(['citadel_koth_enabled'])
  expect(lines[0]).toBe('> sv_hibernate_when_empty 0')
  expect(lines.at(-1)).toBe('> citadel_allow_duplicate_heroes 1')
  expect(lines).toHaveLength(Object.keys(arena).length + 1)

  // A probe logs each step it waits on once, then its verdict, and quits.
  lines.length = 0
  const run = new ProbeRun('Duel', 10)
  let fighters = 0
  for (const now of [1, 1.5, 2]) {
    run.tick(now, 'two fighters', () => fighters >= 2)
    fighters++
  }
  run.tick(3, 'two fighters', () => fighters >= 2)
  run.tick(4, 'two fighters', () => true)
  expect(lines).toEqual([
    'probe: waiting for two fighters',
    'Duel probe PASS',
    '> quit',
  ])

  // A run that outlasts its timeout fails with the step and its state.
  lines.length = 0
  const late = new ProbeRun('Duel', 5)
  late.tick(0, 'a kill', () => false)
  late.tick(
    6,
    'a kill',
    () => false,
    () => '0 kills',
  )
  expect(lines.slice(1)).toEqual([
    'probe failed: timed out waiting for a kill: 0 kills',
    'Duel probe FAIL',
    '> quit',
  ])
})
