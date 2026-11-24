// sticks reads the pilot's four flight axes from the controllers their
// client streams, under the layout the player's "sticks" setting names.
import type { Controller, PlayerControllers } from 'modlock'

import { idle, type Sticks } from './flight'

/** Layout names how the four flight axes sit on a device. */
export type Layout = 'auto' | 'gamepad' | 'aetr' | 'taer'

/**
 * read returns the pilot's sticks under layout, or idle when the stream is
 * not live or no device fits, so the drone holds its attitude with the
 * motors cut.
 */
export function read(
  entry: PlayerControllers | undefined,
  layout: Layout,
): Sticks {
  if (entry?.stream !== 'live') return idle

  // A radio is a joystick that is not a gamepad and has four or more axes.
  const radio = entry.controllers.find(
    (c) => !c.info.gamepad && c.state.axes.length >= 4,
  )
  const pad = entry.controllers.find((c) => c.info.gamepad)
  switch (layout) {
    case 'auto':
      if (radio) return channels(radio, 'aetr')
      return pad ? gamepad(pad) : idle
    case 'gamepad':
      return pad ? gamepad(pad) : idle
    case 'aetr':
    case 'taer':
      return radio ? channels(radio, layout) : idle
  }
}

/**
 * gamepad reads mode 2: the left stick holds throttle (its center is half
 * throttle) and yaw, and the right stick pitch and roll. SDL's y is positive
 * down, so pushing a stick forward is negative.
 */
function gamepad(pad: Controller): Sticks {
  const g = pad.state.gamepad
  return {
    throttle: (1 - g.leftY) / 2,
    yaw: g.leftX,
    pitch: -g.rightY,
    roll: g.rightX,
  }
}

/**
 * channels reads a radio in USB joystick mode, whose first four axes are its
 * first four channels: aileron (roll), elevator (pitch), throttle and rudder
 * (yaw) in the order layout names. Each channel runs from -1 at its low end
 * to 1 at its high end, so throttle maps from the bottom of its travel.
 */
function channels(radio: Controller, layout: 'aetr' | 'taer'): Sticks {
  const a = radio.state.axes
  const [roll, pitch, throttle, yaw] =
    layout === 'aetr' ? [a[0], a[1], a[2], a[3]] : [a[1], a[2], a[0], a[3]]
  return { throttle: (throttle + 1) / 2, roll, pitch, yaw }
}
