import { expect, test } from 'bun:test'

import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import { bind } from '../src/host.js'
import { logAsync } from '../src/host.gen.js'

/** methods holds the method of each call the host took, in order. */
const methods: string[] = []

// A host from before Submit answers it as it answers any name it lacks.
bind((bytes) => {
  const sent = Call.fromBinary(bytes)
  methods.push(sent.method ?? '')
  if (sent.method === 'Submit') {
    return Reply.toBinary({ error: 'this host has no method Submit' })
  }
  return Reply.toBinary({})
})

test('a host without Submit runs async calls as synchronous ones, asked once', async () => {
  // The promises settle with the calls' results and no error reaches the mod.
  expect(await Promise.all([logAsync('a'), logAsync('b')])).toEqual([
    true,
    true,
  ])
  expect(methods).toEqual(['Submit', 'Log', 'Log'])

  // The host is not asked again.
  expect(await logAsync('c')).toBe(true)
  expect(methods).toEqual(['Submit', 'Log', 'Log', 'Log'])
})
