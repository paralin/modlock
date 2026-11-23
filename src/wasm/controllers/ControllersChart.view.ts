// One player's controllers as a mod reads them: nothing until the player's
// client answers the watch, then live, unfocused or stale, and nothing again
// when the watch ends or the link drops.

type Controllers = {
  watched: boolean
  synced: boolean
  stale: boolean
  hFocus: boolean
  up: boolean
  open: boolean
}

export const title = 'Controllers reached through the client'

export const state = (v: Controllers) => {
  // The watch outranks the link, and the link outranks the stream's health.
  if (!v.watched)
    return v.open ? 'Not watched: the client still reading' : 'Not watched'
  if (!v.synced)
    return v.up
      ? 'Watched: waiting for the client'
      : 'Watched: no client linked'
  if (v.stale) return 'Stale: last state kept'
  if (!v.hFocus) return 'Unfocused: nothing held'
  return 'Live'
}

const steps: Record<string, string> = {
  Watch: 'a mod watches the player',
  Unwatch: 'the watch ends',
  Connect: "the player's client links",
  Lose: 'the link drops or the client exits',
  Close: 'the client closes the devices',
  Receive: 'a frame arrives',
  Deadline: 'no frame before the deadline',
}

export const step = (action: string, from: Controllers, to: Controllers) => {
  if (state(from) === state(to)) return null
  return steps[action] ?? null
}
