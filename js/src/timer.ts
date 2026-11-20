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
//
// The clock keeps its timers in a binary min-heap ordered by due time, then by
// the order they were scheduled, so a frame costs O(1) when nothing is due and
// O(k log n) when k of n timers are. A cancelled timer stays in the heap until
// it comes due and is then dropped.

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
  /** at is the game time the entry next runs. */
  at = 0
  /** order breaks ties in at, so timers due together run in schedule order. */
  order = 0

  /**
   * run is the entry's callback. It returns the delay in seconds before its
   * next run, or undefined when it has run for the last time.
   */
  constructor(
    readonly delay: number,
    readonly run: () => number | undefined,
  ) {}

  cancel(): void {
    this.finished = true
  }
}

/** now is the game time of the latest frame, unset before the first. */
let now: number | undefined

/** waiting holds the timers made before the first frame set the clock. */
let waiting: Entry[] = []

/** heap holds the scheduled timers, the earliest due at index 0. */
const heap: Entry[] = []

/** scheduled counts every push onto the heap, numbering each entry's order. */
let scheduled = 0

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
  if (now === undefined) {
    waiting.push(entry)
  } else {
    push(entry, now + delay)
  }
  return entry
}

/**
 * advance moves the clock to a frame and runs the timers that came due. A
 * callback that throws ends only its own timer. A clock that runs backward,
 * as a new map's does, keeps each timer's remaining delay.
 */
export function advance(seconds: number): void {
  // Carry the timers across a clock that went back. The shift moves every
  // entry alike, so the heap keeps its order.
  if (now !== undefined && seconds < now) {
    const shift = seconds - now
    for (const entry of heap) {
      entry.at += shift
    }
  }

  // Record the clock and count the first delays of the timers made before it
  // was known.
  now = seconds
  if (waiting.length !== 0) {
    for (const entry of waiting) {
      push(entry, seconds + entry.delay)
    }
    waiting = []
  }

  // Take the due timers before running any, so a timer a callback makes waits
  // for the next frame.
  const due: Entry[] = []
  for (
    let top = heap[0];
    top !== undefined && top.at <= seconds;
    top = heap[0]
  ) {
    pop()
    if (!top.finished) {
      due.push(top)
    }
  }

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
      } else if (!entry.finished) {
        const next = entry.at + wait
        push(entry, next > seconds ? next : seconds + wait)
      }
    } catch (error) {
      entry.finished = true
      log(`timer failed: ${String(error)}`)
    }
  }
}

/** push schedules entry to run at the game time at. */
function push(entry: Entry, at: number): void {
  // Number the entry and add it as the last leaf.
  entry.at = at
  entry.order = scheduled++
  heap.push(entry)

  // Sift the new entry up past every later parent; the root has none.
  let index = heap.length - 1
  for (;;) {
    const parent = (index - 1) >> 1
    const above = heap[parent]
    if (index === 0 || above === undefined || !before(entry, above)) {
      break
    }
    heap[index] = above
    index = parent
  }
  heap[index] = entry
}

/** pop removes the earliest due entry, if any. */
function pop(): void {
  // Take the last leaf; with nothing else left, the root went with it.
  const last = heap.pop()
  if (last === undefined || heap.length === 0) {
    return
  }

  // Move the last entry to the root, then sift it down past every earlier
  // child.
  let index = 0
  for (;;) {
    let child = 2 * index + 1
    let below = heap[child]
    const right = heap[child + 1]
    if (below !== undefined && right !== undefined && before(right, below)) {
      child++
      below = right
    }
    if (below === undefined || !before(below, last)) {
      break
    }
    heap[index] = below
    index = child
  }
  heap[index] = last
}

/** before reports whether a runs ahead of b. */
function before(a: Entry, b: Entry): boolean {
  return a.at < b.at || (a.at === b.at && a.order < b.order)
}
