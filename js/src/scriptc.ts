// The library's event export as a scriptc build calls it. The build's entry
// declares the host callbacks, which scriptc binds only there, and passes
// them to event on each Call.

import { bind, deliver } from './host.js'
import './index.js'

/**
 * event answers the Call the host holds, size bytes long. call hands the
 * host an encoded Call and returns the size of its Reply; read copies the
 * bytes the host holds, the Call first and then each Reply, into its
 * argument.
 */
export function event(
  size: number,
  call: (data: Uint8Array) => number,
  read: (data: Uint8Array) => void,
): Uint8Array {
  // Read the Call before binding, since binding sends the calls the module
  // made as it loaded and each reply replaces the bytes the host holds.
  const request = new Uint8Array(size)
  read(request)
  bind((data) => {
    const reply = new Uint8Array(call(data))
    read(reply)
    return reply
  })
  return deliver(request)
}
