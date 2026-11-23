import { expect, test } from 'bun:test'

import {
  EntityOptions,
  EntityResponse,
} from '../../src/wasm/entities/entities.pb.js'
import {
  PawnResponse,
  PawnsResponse,
  PlayerRequest,
} from '../../src/wasm/players/players.pb.js'
import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import { CCitadel_BreakableProp, create } from '../src/entities.js'
import { type Pawn, Player } from '../src/host.gen.js'
import { bind, call, deliver } from '../src/host.js'
import { onFrame } from '../src/index.js'
import { setPawnCache } from '../src/pawns.js'

/** answer is what the bound transport replies with next. */
let answer = Reply.toBinary({})

/** sent holds the Calls the transport carried, decoded. */
const sent: { method: string; request: Uint8Array }[] = []
bind((bytes) => {
  const decoded = Call.fromBinary(bytes)
  sent.push({
    method: decoded.method ?? '',
    request: decoded.request ?? new Uint8Array(),
  })
  return answer
})

test('a call frames its method and request as the generated codec reads them', () => {
  // Requests across the one and two byte length boundary, and an empty one.
  for (const size of [0, 1, 127, 128, 300, 20000]) {
    const request = new Uint8Array(size).map((_, i) => i & 0xff)
    sent.length = 0
    call('Pawn', request)
    expect(sent).toEqual([{ method: 'Pawn', request }])
  }
})

test('a method name outside ASCII still reaches the host intact', () => {
  sent.length = 0
  call('Pawné', new Uint8Array([1, 2]))
  expect(sent).toEqual([{ method: 'Pawné', request: new Uint8Array([1, 2]) }])
})

test('a reply returns its response, or undefined when it names an error', () => {
  // A response comes back as its bytes.
  answer = Reply.toBinary({ response: new Uint8Array([9, 8, 7]) })
  expect(Array.from(call('Pawn', new Uint8Array())!)).toEqual([9, 8, 7])

  // A reply without a response is an empty success.
  answer = Reply.toBinary({})
  expect(call('Pawn', new Uint8Array())!.length).toBe(0)

  // An error fails the call even beside a response.
  answer = Reply.toBinary({ error: 'no pawn', response: new Uint8Array([1]) })
  expect(call('Pawn', new Uint8Array())).toBeUndefined()

  // A long response takes a multi-byte length.
  const long = new Uint8Array(1000).fill(5)
  answer = Reply.toBinary({ response: long })
  expect(call('Pawn', new Uint8Array())).toEqual(long)
})

test('create types each key by its own table or a base, and fills the subclass', () => {
  // The host answers with the new entity's handle.
  sent.length = 0
  answer = Reply.toBinary({ response: EntityResponse.toBinary({ entity: 42 }) })
  const prop = create('citadel_breakable_prop', {
    position: { x: 1, y: 2, z: 3 },
    keys: { health: 50, gravity: 0.5 },
  })
  expect(prop).toBeInstanceOf(CCitadel_BreakableProp)
  expect(prop?.handle).toBe(42)

  // health and gravity come from CBaseEntity's table, at the root of the
  // prop's bases.
  const options = EntityOptions.fromBinary(sent[0]!.request)
  expect(options.subclass).toBe('citadel_breakable_prop_vase')
  expect(options.keyValues).toEqual([
    { key: 'health', value: { value: { case: 'integer', value: 50 } } },
    { key: 'gravity', value: { value: { case: 'number', value: 0.5 } } },
  ])
  answer = Reply.toBinary({})
})

test('a pawn read keeps what its event read until the game may change', () => {
  // The stand-in game holds each slot's hero health and answers Pawn and
  // Pawns from it; SetStamina stands for any call that may write.
  const health = new Map([
    [1, 100],
    [2, 200],
  ])
  const pawn = (slot: number) => {
    const h = health.get(slot)
    return h === undefined ? undefined : { entity: 10 + slot, health: h }
  }
  bind((bytes) => {
    const c = Call.fromBinary(bytes)
    sent.push({
      method: c.method ?? '',
      request: c.request ?? new Uint8Array(),
    })
    switch (c.method) {
      case 'Pawn': {
        const slot =
          PlayerRequest.fromBinary(c.request ?? new Uint8Array()).player ?? 0
        return Reply.toBinary({
          response: PawnResponse.toBinary({ pawn: pawn(slot) }),
        })
      }
      case 'Pawns':
        return Reply.toBinary({
          response: PawnsResponse.toBinary({
            pawns: [...health.keys()].map((player) => ({
              player,
              pawn: pawn(player),
            })),
          }),
        })
    }
    return Reply.toBinary({})
  })
  const methods = () => sent.splice(0).map((s) => s.method)
  const one = Player.of(1)
  const two = Player.of(2)
  const three = Player.of(3)
  let inside: () => void = () => {}
  onFrame(() => inside())
  const event = (body: () => void) => {
    inside = () => {
      sent.length = 0
      body()
    }
    deliver(Call.toBinary({ method: 'Frame' }))
    inside = () => {}
  }
  sent.length = 0

  // Outside an event every read reaches the host.
  one.pawn()
  one.pawn()
  expect(methods()).toEqual(['Pawn', 'Pawn'])

  let first: Pawn | undefined
  event(() => {
    // The first slot takes its own call and is answered again after it.
    first = one.pawn()
    expect(one.pawn()).toBe(first)
    expect(methods()).toEqual(['Pawn'])

    // A second slot reads every slot at once, and the batch answers the rest,
    // a slot with no pawn included.
    expect(two.pawn()?.health).toBe(200)
    expect(three.pawn()).toBeUndefined()
    expect(one.pawn()).toBe(first)
    expect(methods()).toEqual(['Pawns'])

    // A call that may write drops the reads; unchanged bytes keep their Pawn,
    // and changed ones decode anew.
    health.set(2, 150)
    one.setStamina(50)
    expect(one.pawn()).toBe(first)
    expect(two.pawn()?.health).toBe(150)
    expect(methods()).toEqual(['SetStamina', 'Pawn', 'Pawns'])
  })

  // The next event reads again, since the game ran between them.
  health.set(1, 90)
  event(() => {
    expect(one.pawn()?.health).toBe(90)
    expect(one.pawn()).not.toBe(first)
    expect(methods()).toEqual(['Pawn'])
  })

  // With the cache off, every read in an event reaches the host.
  setPawnCache(false)
  event(() => {
    one.pawn()
    one.pawn()
    expect(methods()).toEqual(['Pawn', 'Pawn'])
  })
  setPawnCache(true)
})
