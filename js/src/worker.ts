// The entry of a script a worker runs, which mods import as modlock/worker.
// A worker is a second interpreter of the mod's own, on a thread of the host's
// beside the server's tick, with no game to talk to: it may log and read the
// mod's files, and the host refuses every other call. It answers one request
// at a time, in the order the mod made them, and keeps its state between
// them, so a table it decoded once can answer many small questions. A request
// that runs out of time or memory loses that state, and the next request
// starts the script again.
//
//   import { serve } from 'modlock/worker'
//   import { answer } from './table.js'
//
//   serve(answer)
//
// workers.ts is the mod's side.

import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import {
  bind,
  deliver,
  serveEvents,
  type Dispatch,
  type Transport,
} from './host.js'

export { log, readFile, readFileBytes } from './host.gen.js'

/** Handle answers one request with the bytes the mod receives. */
export type Handle = (request: Uint8Array) => Uint8Array

/** Bridge is what the QuickJS runtime installs as __modlock. */
interface Bridge {
  hostCall: Transport
  event?: Dispatch
}

/**
 * serve answers each request the mod sends this worker with handle. A
 * request whose handle throws ends the mod's call with the error, and the
 * worker answers the next one.
 */
export function serve(handle: Handle): void {
  // Send host calls through the interpreter's bridge, then answer the
  // host's events with handle.
  const bridge = (globalThis as unknown as { __modlock: Bridge }).__modlock
  const host: Transport = (call) => bridge.hostCall(call)
  bind(host)
  serveEvents((encoded) => {
    // The host starts the worker once, then sends each request.
    const call = Call.fromBinary(encoded)
    if (call.method !== 'Request') {
      return Reply.toBinary({})
    }
    try {
      return Reply.toBinary({
        response: handle(call.request ?? new Uint8Array()),
      })
    } catch (error) {
      return Reply.toBinary({ error: String(error) })
    }
  })
  bridge.event = (call) => {
    bind(host)
    return deliver(call)
  }
}
