// Numbers from a mod's tree may be unset, infinite or not a number, so the
// drawing reads each through finite before it lays out or sends anything.

/** finite returns value, or zero when it is unset or not a finite number. */
export function finite(value: number | undefined): number {
  return value !== undefined && Number.isFinite(value) ? value : 0
}

/** clamp keeps value within [low, high]. */
export function clamp(value: number, low: number, high: number): number {
  return Math.min(high, Math.max(low, finite(value)))
}

/**
 * Track is the bounds a slider or a range shares: min to max, and the step
 * between the values it stops at.
 */
export interface Track {
  min?: number
  max?: number
  step?: number
}

/** trackRange returns the mod's finite bounds, or the default 0 to 1. */
export function trackRange(track: Track): [number, number] {
  const min = finite(track.min)
  const max = finite(track.max)
  return max > min ? [min, max] : [0, 1]
}

/** trackFraction maps a value to its share of the track, from 0 to 1. */
export function trackFraction(track: Track, value: number): number {
  const [min, max] = trackRange(track)
  return clamp((finite(value) - min) / (max - min), 0, 1)
}

/** trackValue maps a share of the track to a value, snapped to its step. */
export function trackValue(track: Track, fraction: number): number {
  const [min, max] = trackRange(track)
  const value = min + clamp(fraction, 0, 1) * (max - min)
  const step = finite(track.step)
  return step > 0
    ? clamp(min + Math.round((value - min) / step) * step, min, max)
    : value
}
