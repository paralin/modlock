// A mod moves work that touches no game state, such as decoding a table or
// searching it, off the server's tick with a worker: a script of the mod's
// own that the host runs on another thread (worker.ts).
//
//   import { worker } from 'modlock'
//   import { answer } from './table.js'
//
//   const table = worker('table.worker.js', answer)
//   const spot = await table.call(question)
//
// The answer arrives in a later frame. It is copied into the mod on the tick,
// at about 2 ms a megabyte, so a worker should keep large state to itself and
// answer with what the mod needs now.
//
// A host that runs no workers for the mod refuses the call as unavailable.
// call then runs handle in the mod itself, as a task on the next frame, and
// does so for every later call, so a mod behaves the same either way and only
// the tick pays for the work. handle is the same function the worker serves,
// which is why the mod passes it.

import { callServiceLater } from './service-later.js'
import { task } from './tasks.js'

/** workerService is the host service that carries a mod's worker calls. */
const workerService = 'modlock.worker'

/** Worker is a mod's handle on one worker script. */
export interface Worker {
  /** call sends request to the worker and resolves with its answer. */
  call(request: Uint8Array): Promise<Uint8Array>
}

/**
 * worker returns a handle on the worker that runs script, a built file of the
 * mod, starting it on the first call. handle is the function script serves,
 * which the mod runs itself when the host gives it no worker.
 */
export function worker(
  script: string,
  handle: (request: Uint8Array) => Uint8Array,
): Worker {
  let inline = false
  const runInline = (request: Uint8Array) =>
    new Promise<Uint8Array>((resolve, reject) => {
      task(() => {
        try {
          resolve(handle(request))
        } catch (error) {
          reject(error)
        }
      })
    })
  return {
    call(request) {
      if (inline) {
        return runInline(request)
      }
      return new Promise((resolve, reject) => {
        callServiceLater(
          workerService,
          script,
          request,
          (reply, error, failure) => {
            if (reply !== undefined) {
              resolve(reply)
            } else if (failure === 'unavailable') {
              inline = true
              runInline(request).then(resolve, reject)
            } else {
              reject(new Error(`worker ${script}: ${error ?? 'no answer'}`))
            }
          },
        )
      })
    },
  }
}
