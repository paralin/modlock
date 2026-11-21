import { expect, test } from 'bun:test'

import { GameRecord } from '../../src/wasm/records/records.pb.js'
import {
  Call,
  LogRequest,
  Reply,
  ServiceCall,
  ServiceReply,
} from '../../src/wasm/wasm.pb.js'
import { Player } from '../src/host.gen.js'
import { bind } from '../src/host.js'
import { record } from '../src/records.js'

test('record encodes each value in the case its type takes', () => {
  // Answer the records service and keep what it was handed.
  const added: GameRecord[] = []
  bind((bytes) => {
    const call = Call.fromBinary(bytes)
    const service = ServiceCall.fromBinary(call.request)
    expect([call.method, service.service, service.method]).toEqual([
      'CallService',
      'records',
      'Add',
    ])
    added.push(GameRecord.fromBinary(service.payload))
    return Reply.toBinary({ response: ServiceReply.toBinary({}) })
  })

  // Record a find with every kind of value, and one left out.
  const taken = record('find', {
    round: 3,
    seconds: 41.2,
    big: 9007199254740993n,
    prop: 'barrel',
    first: true,
    seeker: Player.of(2),
    hider: undefined,
  })
  expect(taken).toBe(true)
  expect(added).toHaveLength(1)
  expect(added[0]).toEqual({
    kind: 'find',
    fields: {
      round: { value: { case: 'integer', value: 3n } },
      seconds: { value: { case: 'number', value: 41.2 } },
      big: { value: { case: 'integer', value: 9007199254740993n } },
      prop: { value: { case: 'text', value: 'barrel' } },
      first: { value: { case: 'bool', value: true } },
      seeker: { value: { case: 'player', value: 2 } },
    },
  })
})

test('record logs a record no service takes', () => {
  // Refuse the service call and keep the log line.
  const logged: string[] = []
  bind((bytes) => {
    const call = Call.fromBinary(bytes)
    if (call.method === 'Log') {
      logged.push(LogRequest.fromBinary(call.request).message ?? '')
      return Reply.toBinary({})
    }
    return Reply.toBinary({ error: 'this host provides no service records' })
  })

  expect(record('round', { area: 'docks', round: 3 })).toBe(false)
  expect(
    logged.some((line) => line.includes('record round: area="docks" round=3')),
  ).toBe(true)
})
