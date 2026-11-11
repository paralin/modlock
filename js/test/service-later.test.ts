import { expect, test } from 'bun:test'

import {
  Call,
  Reply,
  ServiceCallStarted,
  ServiceFailure,
  ServiceRepliedEvent,
  StartServiceCallRequest,
} from '../../src/wasm/wasm.pb.js'
import { bind, deliver as dispatch } from '../src/host.js'
import { callServiceLater } from '../src/index.js'

/** started holds the calls the mod started, in order. */
const started: { service: string; method: string; timeout: number }[] = []

// The host numbers each started call from one.
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
    timeout: request.timeoutMilliseconds ?? 0,
  })
  const call = BigInt(started.length)
  return Reply.toBinary({ response: ServiceCallStarted.toBinary({ call }) })
})

/** replied delivers one ServiceReplied event as the host does. */
function replied(
  event: Parameters<typeof ServiceRepliedEvent.toBinary>[0],
): void {
  const reply = Reply.fromBinary(
    dispatch(
      Call.toBinary({
        method: 'ServiceReplied',
        request: ServiceRepliedEvent.toBinary(event),
      }),
    ),
  )
  expect(reply.error ?? '').toBe('')
}

test('a started call ends once, through the event carrying its number', () => {
  // Start an answered call and a refused one.
  const ended: string[] = []
  callServiceLater(
    'example.drills.v1',
    'Drill',
    new Uint8Array([1]),
    (reply, error, failure) => {
      ended.push(
        error
          ? `${failure}: ${error}`
          : `reply ${Array.from(reply ?? []).join(',')}`,
      )
    },
  )
  callServiceLater(
    'example.drills.v1',
    'Drill',
    new Uint8Array(),
    (reply, error, failure) =>
      ended.push(error ? `${failure}: ${error}` : 'reply'),
    500,
  )
  expect(started).toEqual([
    { service: 'example.drills.v1', method: 'Drill', timeout: 0 },
    { service: 'example.drills.v1', method: 'Drill', timeout: 500 },
  ])
  expect(ended).toEqual([])

  // Each ends with its own event, and a repeated or unknown number does
  // nothing.
  replied({
    call: 2n,
    failure: ServiceFailure.TIMEOUT,
    error: 'too slow',
  })
  replied({ call: 1n, payload: new Uint8Array([7, 8]) })
  replied({ call: 1n, payload: new Uint8Array([9]) })
  replied({ call: 99n, payload: new Uint8Array([9]) })
  expect(ended).toEqual(['timeout: too slow', 'reply 7,8'])
})
