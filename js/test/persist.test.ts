import { expect, test } from 'bun:test'

import {
  GetManyReply,
  GetManyRequest,
  GetReply,
  GetRequest,
  UpdateReply,
  UpdateRequest,
} from '../../src/wasm/persist/persist.pb.js'
import {
  Call,
  Reply,
  ServiceCallStarted,
  ServiceFailure,
  ServiceRepliedEvent,
  StartServiceCallRequest,
} from '../../src/wasm/wasm.pb.js'
import { Player } from '../src/host.gen.js'
import { bind, deliver as dispatch } from '../src/host.js'
import { persist } from '../src/index.js'

/** started holds the calls the mod started, numbered from one. */
const started: { service: string; method: string; payload: Uint8Array }[] = []

bind((bytes) => {
  const sent = Call.fromBinary(bytes)
  if (sent.method !== 'StartServiceCall') {
    return Reply.toBinary({})
  }
  const request = StartServiceCallRequest.fromBinary(
    sent.request ?? new Uint8Array(),
  )
  started.push({
    service: request.service ?? '',
    method: request.method ?? '',
    payload: request.payload ?? new Uint8Array(),
  })
  const call = BigInt(started.length)
  return Reply.toBinary({ response: ServiceCallStarted.toBinary({ call }) })
})

/** replied ends the started call numbered call, as the host does. */
function replied(
  call: number,
  event: Omit<Parameters<typeof ServiceRepliedEvent.toBinary>[0], 'call'>,
): void {
  const reply = Reply.fromBinary(
    dispatch(
      Call.toBinary({
        method: 'ServiceReplied',
        request: ServiceRepliedEvent.toBinary({ call: BigInt(call), ...event }),
      }),
    ),
  )
  expect(reply.error ?? '').toBe('')
}

/** number is a record value holding n. */
function number(n: number) {
  return { value: { case: 'number' as const, value: n } }
}

test('a record loads once, then answers what the host sent', () => {
  // The first get starts one read and answers loading; a second waits on it.
  const player = Player.of(2)
  expect(persist.get(player, 'stats')).toEqual({ state: 'loading' })
  expect(persist.get(player, 'stats')).toEqual({ state: 'loading' })
  const at = started.length
  expect(started[at - 1]!.service).toBe('persist')
  expect(started[at - 1]!.method).toBe('Get')
  expect(GetRequest.fromBinary(started[at - 1]!.payload)).toEqual({
    player: 2,
    type: 'stats',
  })

  // The answer arrives in a later frame.
  replied(at, {
    payload: GetReply.toBinary({
      record: {
        fields: {
          roundsWon: number(4),
          unlocks: { value: { case: 'tags', value: { members: ['hat'] } } },
        },
      },
    }),
  })
  expect(persist.get(player, 'stats')).toEqual({
    state: 'loaded',
    record: { roundsWon: 4, unlocks: ['hat'] },
  })
  expect(started.length).toBe(at)
})

test('an update sends its changes and keeps the record the host answers', () => {
  // The changes go in field order, a list of them in its order.
  const player = Player.of(5)
  const ended: string[] = []
  persist.update(
    player,
    'stats',
    {
      roundsWon: { add: 1 },
      best: { max: 31.5 },
      title: { set: 'hider' },
      unlocks: [{ insert: 'cone' }, { remove: 'hat' }],
    },
    { key: 'round-9' },
    (record, error, repeated) =>
      ended.push(
        error ??
          `${Object.keys(record ?? {}).join(',')} ${String(record?.roundsWon)} ${repeated}`,
      ),
  )
  const update = started.length
  const request = UpdateRequest.fromBinary(started[update - 1]!.payload)
  expect(started[update - 1]!.method).toBe('Update')
  expect([request.player, request.type, request.updateKey]).toEqual([
    5,
    'stats',
    'round-9',
  ])
  expect(request.ops).toEqual([
    { field: 'roundsWon', op: { case: 'add', value: 1 } },
    { field: 'best', op: { case: 'max', value: 31.5 } },
    {
      field: 'title',
      op: { case: 'set', value: { value: { case: 'text', value: 'hider' } } },
    },
    { field: 'unlocks', op: { case: 'insert', value: 'cone' } },
    { field: 'unlocks', op: { case: 'remove', value: 'hat' } },
  ])

  // The answer becomes the record get answers.
  replied(update, {
    payload: UpdateReply.toBinary({
      record: { fields: { roundsWon: number(1) } },
      repeated: true,
    }),
  })
  expect(ended).toEqual(['roundsWon 1 true'])
  expect(persist.get(player, 'stats')).toEqual({
    state: 'loaded',
    record: { roundsWon: 1 },
  })
})

test('an answer to an earlier read does not replace a later update', () => {
  // A read starts, then an update, and the update's answer arrives first.
  const player = Player.of(6)
  persist.get(player, 'stats')
  const read = started.length
  persist.update(player, 'stats', { roundsWon: { add: 1 } })
  const update = started.length
  replied(update, {
    payload: UpdateReply.toBinary({
      record: { fields: { roundsWon: number(8) } },
    }),
  })
  replied(read, {
    payload: GetReply.toBinary({
      record: { fields: { roundsWon: number(7) } },
    }),
  })
  expect(persist.get(player, 'stats')).toEqual({
    state: 'loaded',
    record: { roundsWon: 8 },
  })
})

test('a refused read is unavailable, and refresh reads again', () => {
  const player = Player.of(7)
  persist.get(player, 'stats')
  replied(started.length, {
    failure: ServiceFailure.UNAVAILABLE,
    error: 'nobody provides persist',
  })
  expect(persist.get(player, 'stats')).toEqual({
    state: 'unavailable',
    error: 'nobody provides persist',
  })
  persist.refresh(player, 'stats')
  replied(started.length, { payload: GetReply.toBinary({}) })
  expect(persist.get(player, 'stats')).toEqual({ state: 'loaded', record: {} })
})

test('getMany fills each player, and a new player in a slot reads anew', () => {
  // One call reads both players.
  const first = Player.of(8)
  const second = Player.of(9)
  persist.getMany([first, second], 'stats')
  const many = started.length
  expect(GetManyRequest.fromBinary(started[many - 1]!.payload)).toEqual({
    players: [8, 9],
    type: 'stats',
  })
  replied(many, {
    payload: GetManyReply.toBinary({
      records: [
        { player: 8, record: { fields: { roundsWon: number(2) } } },
        { player: 9, record: {} },
      ],
    }),
  })
  expect(persist.get(first, 'stats')).toEqual({
    state: 'loaded',
    record: { roundsWon: 2 },
  })
  expect(persist.get(second, 'stats')).toEqual({ state: 'loaded', record: {} })

  // A player who takes slot 8 next is a new Player, whose record loads.
  Player.release(8)
  expect(persist.get(Player.of(8), 'stats')).toEqual({ state: 'loading' })
})
