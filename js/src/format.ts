// HUD text: the numbers a mode's HUD and chat lines show, written the way
// players read them. The game's scripts have no locale data, so
// toLocaleString and Intl throw there; these group digits themselves.
//
// Each call is a few string operations, cheap enough to run every frame,
// though a HUD that redraws only on change calls them only then.

/**
 * clock writes seconds as minutes and seconds, such as 4:07, rounding up so a
 * countdown reads 0:00 only once it has run out. Negative time reads 0:00.
 */
export function clock(seconds: number): string {
  const whole = Math.max(0, Math.ceil(seconds))
  return `${Math.floor(whole / 60)}:${String(whole % 60).padStart(2, '0')}`
}

/**
 * stopwatch writes seconds with hundredths, such as 1:04.37, rounding down
 * so a run's time never reads faster than it was. Negative time reads 0:00.00.
 */
export function stopwatch(seconds: number): string {
  const hundredths = Math.max(0, Math.floor(seconds * 100))
  const minutes = Math.floor(hundredths / 6000)
  const rest = (hundredths % 6000) / 100
  return `${minutes}:${rest.toFixed(2).padStart(5, '0')}`
}

/**
 * percent writes part of whole as a whole percentage from 0% to 100%, such
 * as 42%, or a dash while whole is not positive. A fraction is percent(f, 1).
 */
export function percent(part: number, whole: number): string {
  if (!(whole > 0)) {
    return '-'
  }
  return `${Math.round(Math.max(0, Math.min(part / whole, 1)) * 100)}%`
}

/** grouped writes a number rounded to a whole with comma thousands, such as 4,210. */
export function grouped(value: number): string {
  return String(Math.round(value)).replace(/\B(?=(\d{3})+(?!\d))/g, ',')
}

/** ordinal writes a place, such as 1st, 2nd, 11th or 23rd. */
export function ordinal(place: number): string {
  const tens = place % 100
  const suffix =
    tens >= 11 && tens <= 13
      ? 'th'
      : (['th', 'st', 'nd', 'rd'][place % 10] ?? 'th')
  return `${place}${suffix}`
}

/**
 * count writes a number of things, in the plural unless one, such as 1 kill
 * or 3 kills. plural defaults to thing with an s.
 */
export function count(
  number: number,
  thing: string,
  plural = `${thing}s`,
): string {
  return `${grouped(number)} ${number === 1 ? thing : plural}`
}
