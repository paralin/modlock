// A Modlock play session as the player lives it: the mods mount, the host
// starts, the game launches on its first Ready, and Stop or the host's exit
// ends the session with the mods unmounted.

type Session = {
  host: string
  client: boolean
  stopPc: string
  waitPc: string
}

export const title = 'Modlock play session'

export const state = (v: Session) => {
  // Ending outranks stopping, and stopping outranks the host's own state.
  if (v.waitPc === 'done') return 'Ended: mods unmounted'
  if (v.stopPc === 'unmount') return 'Stopping: unmounting the mods'
  if (v.stopPc === 'kill') return 'Stopping: ending the host'
  if (v.host === 'dead') return 'Host gone: Wait cleans up'
  if (v.client) return 'Playing: the game runs'
  if (v.host === 'up') return 'Host up: waiting for Ready'
  return 'Starting: mods mounted, host starting'
}

const steps: Record<string, string> = {
  Connect: 'the host dials the control link',
  Exit: 'the host exits or crashes',
  StopBegin: 'the player ends the session',
  StopPending: "Start's context ends",
  StopUnmount: 'Stop unmounts the mods',
  StopKill: 'Stop ends the host',
  WaitDrain: "the link's last events arrive",
}

// Each arrow changes what the player waits for; a step inside one state, such
// as Wait closing the interfaces while the host is gone, draws nothing.
export const step = (action: string, from: Session, to: Session) => {
  if (state(from) === state(to)) return null
  if (action === 'Handle') return 'Ready arrives and the game launches'
  return steps[action] ?? null
}
