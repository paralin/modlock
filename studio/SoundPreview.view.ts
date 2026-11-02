export const title = 'Sound preview'
// state labels the selected sound's playback state.
export const state = (value: { phase: string }) => value.phase
// step labels only transitions the creator sees.
export const step = (action: string) => action
