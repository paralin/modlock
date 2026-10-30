import { expect, test } from 'bun:test'

import {
  EntityOptions,
  EntityResponse,
} from '../../src/wasm/entities/entities.pb.js'
import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import { CCitadel_BreakableProp, create } from '../src/entities.js'
import { bind, call } from '../src/host.js'

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
