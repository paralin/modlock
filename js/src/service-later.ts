// A started service call ends in a later frame, with the ServiceReplied event
// carrying its number.

import {
  type ServiceFailure,
  type ServiceRepliedEvent,
  startServiceCall,
} from './host.gen.js'
import { after } from './timer.js'

/** waiting maps each started service call's number to the function its end runs. */
const waiting = new Map<bigint, ServiceDone>()

/**
 * ServiceDone ends one callServiceLater: with the reply, or without one and
 * with the error and the failure that says why.
 */
export type ServiceDone = (
  reply: Uint8Array | undefined,
  error?: string,
  failure?: ServiceFailure,
) => void

/**
 * callServiceLater calls method of a service the host provides and returns at
 * once; done runs in a later frame with the answer, or with an error when
 * the service refused, the call waited past timeoutMilliseconds or nobody
 * provides the service. Zero waits the host's longest, 30 seconds. done runs
 * once while the mod runs; calls still waiting when the mod stops end with
 * it.
 */
export function callServiceLater(
  service: string,
  method: string,
  payload: Uint8Array,
  done: ServiceDone,
  timeoutMilliseconds = 0,
): void {
  // A call the host refuses to start ends in the next frame as well, so done
  // never runs before this returns.
  const call = startServiceCall(service, method, payload, timeoutMilliseconds)
  if (call === undefined) {
    after(0, () =>
      done(
        undefined,
        `the host did not start the call to ${service}`,
        'unavailable',
      ),
    )
    return
  }
  waiting.set(call, done)
}

/** endServiceCall ends the started call event names, once. */
export function endServiceCall(event: ServiceRepliedEvent): void {
  // Each call ends once; a number the mod no longer waits for is ignored.
  const done = waiting.get(event.call)
  if (!done) {
    return
  }
  waiting.delete(event.call)

  // A failure ends the call without a reply.
  if (event.failure && event.failure !== 'none') {
    done(undefined, event.error, event.failure)
    return
  }
  done(event.payload)
}
