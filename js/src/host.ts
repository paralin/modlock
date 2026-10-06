// The host bridge the QuickJS runtime gives the bundle: calls to the game
// and the events it delivers.

import { Call, Reply } from '../../proto/modlock/wasm.pb.js'

/** ModlockHost is the bridge the QuickJS runtime installs as __modlock. */
interface ModlockHost {
  /** hostCall exchanges an encoded Call for the host's encoded Reply. */
  hostCall(call: Uint8Array): Uint8Array
  /** event receives each encoded Call the host delivers and returns its Reply. */
  event?: (call: Uint8Array) => Uint8Array
}

/** host is the runtime's bridge for the bundle's lifetime. */
export const host = (globalThis as unknown as { __modlock: ModlockHost })
  .__modlock

/**
 * call calls one Host method with its encoded request and returns the
 * encoded response, or undefined when the call failed; the host logs why.
 */
export function call(
  method: string,
  request: Uint8Array,
): Uint8Array | undefined {
  const reply = Reply.fromBinary(
    host.hostCall(Call.toBinary({ method, request })),
  )
  return reply.error ? undefined : (reply.response ?? new Uint8Array())
}
