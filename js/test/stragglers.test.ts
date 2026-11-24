import { expect, test } from 'bun:test'

import { Call, Reply, ServerCommandRequest } from '../../src/wasm/wasm.pb.js'
import { type Connection, Player } from '../src/host.gen.js'
import { bind } from '../src/host.js'
import { keepPlayers } from '../src/players.js'
import { Stragglers } from '../src/kit/stragglers.js'

/** sent lists the server commands and entity listings the sweep made. */
const sent: string[] = []

bind((bytes) => {
  const call = Call.fromBinary(bytes)
  if (call.method === 'ServerCommand') {
    sent.push(ServerCommandRequest.fromBinary(call.request).command ?? '')
  } else if (call.method === 'FindEntities') {
    sent.push('FindEntities')
  }
  return Reply.toBinary({})
})

/** joined is a connection in slot with the given name, generation and readiness. */
function joined(
  slot: number,
  name: string,
  ready: boolean,
  generation = 1,
  bot = false,
): Connection {
  return {
    player: Player.of(slot),
    steamId: bot ? 0n : BigInt(76561198000000000 + slot),
    name,
    bot,
    ready,
    generation,
    away: false,
    roles: [],
  }
}

/** sweep runs one sweep at seconds and returns what it sent. */
function sweep(stragglers: Stragglers, seconds: number): string[] {
  sent.length = 0
  stragglers.sweep(seconds)
  return sent.filter((line) => line !== 'FindEntities')
}

test('a human stuck joining is kicked once past the deadline, and a reconnect gets a new deadline', () => {
  // Slot 1 never finishes joining, slot 2 has joined and slot 3 is a bot
  // the host lists as ready.
  keepPlayers([
    joined(1, 'KiantheK', false),
    joined(2, 'Ready', true),
    joined(3, 'Bot', false, 1, true),
  ])
  const stragglers = new Stragglers({ connectSeconds: 10, heroes: false })
  expect(sweep(stragglers, 0)).toEqual([])
  expect(sweep(stragglers, 9)).toEqual([])
  expect(sweep(stragglers, 10)).toEqual(['kick "KiantheK"'])
  expect(sweep(stragglers, 11)).toEqual([])

  // A new connection in the slot waits its own full deadline.
  keepPlayers([joined(1, 'KiantheK', false, 2)])
  expect(sweep(stragglers, 12)).toEqual([])
  expect(sweep(stragglers, 21)).toEqual([])
  expect(sweep(stragglers, 22)).toEqual(['kick "KiantheK"'])
})

test('a player who finishes joining before the deadline stays', () => {
  keepPlayers([joined(4, 'Slow', false)])
  const stragglers = new Stragglers({ connectSeconds: 10, heroes: false })
  expect(sweep(stragglers, 0)).toEqual([])
  keepPlayers([joined(4, 'Slow', true)])
  expect(sweep(stragglers, 5)).toEqual([])

  // Losing readiness restarts the clock rather than resuming it.
  keepPlayers([joined(4, 'Slow', false)])
  expect(sweep(stragglers, 6)).toEqual([])
  expect(sweep(stragglers, 15)).toEqual([])
  expect(sweep(stragglers, 16)).toEqual(['kick "Slow"'])
})

test('a name that cannot be quoted is not kicked, and zero turns kicking off', () => {
  keepPlayers([joined(5, 'a"b', false), joined(6, '', false)])
  const stragglers = new Stragglers({ connectSeconds: 1, heroes: false })
  sweep(stragglers, 0)
  expect(sweep(stragglers, 1)).toEqual([])

  keepPlayers([joined(7, 'Stuck', false)])
  const never = new Stragglers({ connectSeconds: 0, heroes: false })
  never.sweep(0)
  expect(sweep(never, 1000)).toEqual([])
})

test('heroes are swept by default and not when heroes is false', () => {
  keepPlayers([])
  sent.length = 0
  new Stragglers().sweep(0)
  expect(sent.includes('FindEntities')).toEqual(true)
  sent.length = 0
  new Stragglers({ heroes: false }).sweep(0)
  expect(sent.includes('FindEntities')).toEqual(false)
})
