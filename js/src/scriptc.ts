// The library's event export as a scriptc build calls it. The build's entry
// imports this module before the mod's main module, so the host transport is
// bound while the mod's top level runs, as it is in QuickJS, and a host call
// made there reaches the host.

import { bind, deliver } from './host.js'
import './index.js'

/** host_call hands the host an encoded Call and returns the size of its Reply. */
declare function host_call(data: Uint8Array): number

/** host_read copies the bytes the host holds, the Call first and then each Reply. */
declare function host_read(data: Uint8Array): void

/** host carries a call to the host and reads its reply. */
function host(data: Uint8Array): Uint8Array {
  const reply = new Uint8Array(host_call(data))
  host_read(reply)
  return reply
}

// Bind the host before the mod's main module loads.
bind((data) => host(data))

/**
 * event answers the Call the host holds, size bytes long. Each Call binds the
 * host again, as the QuickJS entry does, so a transport bound during one
 * event lasts only until the next.
 */
export function event(size: number): Uint8Array {
  bind((data) => host(data))
  const request = new Uint8Array(size)
  host_read(request)
  return deliver(request)
}
