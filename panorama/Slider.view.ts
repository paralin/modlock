/** SliderState is the control the player currently sees. */
type SliderState = { mode: string }

export const title = 'Slider input'

/** state describes which input the current slider accepts. */
export const state = (value: SliderState) =>
  ({
    hidden: 'Slider hidden',
    disabled: 'Slider disabled',
    plain: 'Drag to seek',
    zoom: 'Drag to seek or wheel to zoom',
  })[value.mode] ?? 'Slider hidden'

/** step keeps redraws and direct input visible in the chart. */
export const step = (action: string, from: SliderState, to: SliderState) => {
  if (action.startsWith('Draw'))
    return state(from) === state(to) ? null : 'The mod redraws'
  if (action === 'Drag') return 'Drag the thumb'
  if (action === 'Wheel') return 'Wheel over the timeline'
  return null
}
