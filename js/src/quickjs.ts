// The library as the QuickJS runtime loads it. The runtime installs its
// bridge as __modlock before the bundle runs and calls the bridge's event for
// each Call the host delivers.

import { bind, deliver, type Dispatch, type Transport } from './host.js'

export * from './index.js'

/** Bridge is what the QuickJS runtime installs as __modlock. */
interface Bridge {
  /** hostCall exchanges an encoded Call for the host's encoded Reply. */
  hostCall: Transport
  /** event is set by the library to answer each Call the host delivers. */
  event?: Dispatch
}

const bridge = (globalThis as unknown as { __modlock: Bridge }).__modlock

/** host carries a call to the runtime's bridge. */
const host: Transport = (call) => bridge.hostCall(call)

// Each Call the host delivers binds the bridge again, as the scriptc entry
// does, so a transport bound during one event lasts only until the next.
bind(host)
bridge.event = (call) => {
  bind(host)
  return deliver(call)
}
