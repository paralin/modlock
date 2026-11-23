import { expect, test } from 'bun:test'

import {
  Call,
  CompletedEvent,
  Reply,
  SubmitRequest,
  SubmitResponse,
} from '../../src/wasm/wasm.pb.js'
import { bind, call, deliver as dispatch } from '../src/host.js'
import '../src/index.js'
import {
  logAsync,
  readFieldAsync,
  serverCommandAsync,
} from '../src/host.gen.js'

/** submits holds the Submit requests the host took; others, the methods of the calls made outside one. */
const submits: SubmitRequest[] = []
const others: string[] = []

// The host numbers each submitted call from one.
let numbered = 0n
bind((bytes) => {
  const sent = Call.fromBinary(bytes)
  if (sent.method !== 'Submit') {
    others.push(sent.method ?? '')
    return Reply.toBinary({})
  }
  const request = SubmitRequest.fromBinary(sent.request ?? new Uint8Array())
  submits.push(request)
  const calls = (request.calls ?? []).map(() => ++numbered)
  return Reply.toBinary({ response: SubmitResponse.toBinary({ calls }) })
})

/** completed delivers one Completed event as the host does. */
function completed(event: Parameters<typeof CompletedEvent.toBinary>[0]) {
  const reply = Reply.fromBinary(
    dispatch(
      Call.toBinary({
        method: 'Completed',
        request: CompletedEvent.toBinary(event),
      }),
    ),
  )
  expect(reply.error ?? '').toBe('')
}

test('the calls of one turn go in one Submit and settle from one Completed event', async () => {
  const settled: unknown[] = []
  const all = Promise.all([
    logAsync('a'),
    readFieldAsync(1, 'CBaseEntity', 'm_iHealth', 'int32'),
    serverCommandAsync('status'),
  ]).then((results) => settled.push(...results))

  // Nothing is sent before the turn ends, and then one Submit carries all.
  expect(submits.length).toBe(0)
  await Promise.resolve()
  expect(submits.length).toBe(1)
  expect(submits[0]!.calls?.map((c) => c.method)).toEqual([
    'Log',
    'ReadField',
    'ServerCommand',
  ])
  expect(others).toEqual([])
  expect(settled).toEqual([])

  // One event settles every promise, a failed call as its method's failure
  // value, and the continuation runs without another turn of the host.
  completed({
    calls: [
      { call: 1n, reply: {} },
      { call: 2n, reply: { error: 'no such field' } },
      { call: 3n, reply: { error: 'refused' } },
    ],
  })
  await all
  expect(settled).toEqual([true, undefined, false])

  // A number no call holds is ignored.
  completed({ calls: [{ call: 99n, reply: {} }] })
})

test('a synchronous call goes after the async calls made before it', async () => {
  const before = submits.length
  void logAsync('first')
  call('Log', new Uint8Array())
  expect(submits.length).toBe(before + 1)
  expect(others).toEqual(['Log'])
  completed({ calls: [{ call: numbered, reply: {} }] })
  await Promise.resolve()
})
