// A mod schedules work on the game clock:
//
//   import { after, every } from 'modlock'
//
//   after(3, () => log('three seconds later'))
//   const heartbeat = every(1, () => log('tick'))
//   heartbeat.cancel()
//
// Time is the game's, so a paused game holds its timers. A tick is 1/64
// second. Timers run from the frame event, at most once per frame each, in the
// order they came due.

import { log } from './host.gen.js'

/** Timer is one scheduled callback. */
export interface Timer {
  /** finished reports whether the timer ran its last time or was cancelled. */
  readonly finished: boolean
  /** cancel stops the timer; it does nothing once the timer is finished. */
  cancel(): void
}

/** Entry is a Timer the clock keeps until it finishes. */
class Entry implements Timer {
  finished = false
  /** at is the game time the entry next runs, unset until the clock is known. */
  at: number | undefined

  /**
   * run is the entry's callback. It returns the delay in seconds before its
   * next run, or undefined when it has run for the last time.
   */
  constructor(
    private readonly delay: number,
    readonly run: () => number | undefined,
  ) {
    this.at = now === undefined ? undefined : now + delay
  }

  /** start counts the first delay from the frame at seconds, once. */
  start(seconds: number): void {
    this.at ??= seconds + this.delay
  }

  cancel(): void {
    this.finished = true
  }
}

/** now is the game time of the latest frame, unset before the first. */
let now: number | undefined

/** entries are the timers that have not finished. */
let entries: Entry[] = []

/** after runs fn once, seconds from now. */
export function after(seconds: number, fn: () => void): Timer {
  return schedule(seconds, () => {
    fn()
    return undefined
  })
}

/** every runs fn each seconds, keeping its cadence through a late frame. */
export function every(seconds: number, fn: () => void): Timer {
  return schedule(seconds, () => {
    fn()
    return seconds
  })
}

/**
 * sequence calls fn on the next frame with the number of times it has run,
 * starting at 1. fn returns the seconds to wait before it runs again, or
 * nothing to end the sequence.
 */
export function sequence(fn: (run: number) => number | void): Timer {
  let run = 0
  return schedule(0, () => fn(++run) ?? undefined)
}

/** schedule adds a timer whose first run is delay seconds away. */
function schedule(delay: number, run: () => number | undefined): Timer {
  const entry = new Entry(delay, run)
  entries.push(entry)
  return entry
}

/**
 * advance moves the clock to a frame and runs the timers that came due. A
 * callback that throws ends only its own timer. A clock that runs backward,
 * as a new map's does, keeps each timer's remaining delay.
 */
export function advance(seconds: number): void {
  // Carry the timers across a clock that went back.
  if (now !== undefined && seconds < now) {
    const shift = seconds - now
    for (const entry of entries) {
      if (entry.at !== undefined) {
        entry.at += shift
      }
    }
  }

  // Record the clock, start the timers made before it was known, then take the
  // due ones in order. Timers a callback makes wait for the next frame.
  now = seconds
  for (const entry of entries) {
    entry.start(seconds)
  }
  const due = entries
    .filter((entry) => !entry.finished && (entry.at as number) <= seconds)
    .sort((a, b) => (a.at as number) - (b.at as number))

  // Run each one, ending a timer that finishes or throws and moving a repeat to
  // its next run.
  for (const entry of due) {
    if (entry.finished) {
      continue
    }
    try {
      const wait = entry.run()
      if (wait === undefined) {
        entry.finished = true
      } else {
        const next = (entry.at as number) + wait
        entry.at = next > seconds ? next : seconds + wait
      }
    } catch (error) {
      entry.finished = true
      log(`timer failed: ${String(error)}`)
    }
  }
  entries = entries.filter((entry) => !entry.finished)
}
