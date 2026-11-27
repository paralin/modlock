const labels: Record<string, string> = {
  idle: 'Nothing plays',
  running: 'The presentation plays',
  held: 'Held while the game pauses',
  done: 'The presentation finished',
}
const events: Record<string, string | null> = {
  Start: 'the mod starts it',
  Restart: null,
  Cancel: 'the mod cancels it',
  Tick: 'its last step plays',
  Pause: 'the game pauses',
  Resume: 'the game resumes',
}

/** title names what the player watches. */
export const title = 'Timed presentation'

/** state describes what the player sees. */
export const state = (view: { phase: string }) =>
  labels[view.phase] ?? view.phase

/** step labels the event that moves the presentation. */
export const step = (
  action: string,
  from?: { phase: string },
  to?: { phase: string },
) => {
  const name = action.replace(/\(.*$/, '')
  if (name === 'Tick' && from?.phase === to?.phase) return null
  if ((name === 'Pause' || name === 'Resume') && from?.phase === to?.phase)
    return null
  return name in events ? events[name] : action
}
