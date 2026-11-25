// A mod spreads long work across frames with a task, so one frame never holds
// the server's tick:
//
//   import { task } from 'modlock'
//
//   const load = task(async (t) => {
//     for (const row of rows) {
//       decode(row)
//       await t.pace()
//     }
//   })
//   load.cancel()
//
// A task starts on the next frame, after the mod's frame handlers. pace
// returns at once while the frame is within the time the host gave the mod
// and waits for the next frame once it is past it; frame always waits for the
// next frame. Between two frames the mod's event has returned to the host, so
// damage, commands and pings in the ticks between run at once against live
// state, as they do without tasks.
//
// A host that gives the mod no budget sends zero, and pace then never waits:
// a task runs to its end, or to its first frame call, in the frame it starts,
// as a plain loop would.
//
// Each frame resumes the tasks that waited for it in the order they began
// waiting, and they take turns a step at a time. A resumed task always runs
// one step before pace can stop it, so a costly task cannot starve the others;
// the price is that a frame can overrun its budget by one step of each task.
// Tasks.tla models the schedule.
//
// cancel ends a task at its next pace or frame, which rejects, so the task's
// finally blocks run and it runs no further step.

import { log } from './host.gen.js'

/** Task is work a mod spreads across frames. */
export interface Task {
  /** finished reports whether the task ended, by returning, throwing or cancel. */
  readonly finished: boolean
  /** cancel ends the task at its next pace or frame. */
  cancel(): void
  /** pace waits for the next frame once this one is past its budget. */
  pace(): Promise<void> | undefined
  /** frame waits for the next frame. */
  frame(): Promise<void>
}

/** Cancelled rejects the wait of a cancelled task. */
class Cancelled extends Error {
  constructor() {
    super('task cancelled')
  }
}

/** Waiter is a task waiting for the next frame. */
interface Waiter {
  readonly task: Entry
  readonly resume: () => void
  readonly end: (error: Cancelled) => void
}

/** deadline is the runtime clock reading past which the frame is late. */
let deadline = Infinity

/** waiting holds the tasks the next frame resumes, in the order they waited. */
let waiting: Waiter[] = []

/** Entry is a Task the scheduler keeps until it ends. */
class Entry implements Task {
  finished = false
  cancelled = false

  cancel(): void {
    this.cancelled = true
  }

  pace(): Promise<void> | undefined {
    return this.cancelled || late() ? this.frame() : undefined
  }

  frame(): Promise<void> {
    return new Promise((resume, end) =>
      waiting.push({ task: this, resume, end }),
    )
  }
}

/** task starts work on the next frame and returns its handle. */
export function task(fn: (task: Task) => Promise<void> | void): Task {
  const entry = new Entry()
  waiting.push({
    task: entry,
    resume: () => void run(entry, fn),
    end: () => {
      entry.finished = true
    },
  })
  return entry
}

/** run runs a started task to its end, logging a failure. */
async function run(
  entry: Entry,
  fn: (task: Task) => Promise<void> | void,
): Promise<void> {
  try {
    await fn(entry)
  } catch (error) {
    if (!(error instanceof Cancelled)) {
      log(`task failed: ${String(error)}`)
    }
  } finally {
    entry.finished = true
  }
}

/**
 * late reports whether the current frame is past the time the host gave the
 * mod. It is false whenever the host gave none.
 */
export function late(): boolean {
  return performance.now() > deadline
}

/**
 * beginFrame starts the frame's budget, in microseconds, zero for none. The
 * library calls it before the frame's handlers, so they count against it.
 */
export function beginFrame(budgetMicroseconds: number): void {
  deadline =
    budgetMicroseconds === 0
      ? Infinity
      : performance.now() + budgetMicroseconds / 1000
}

/**
 * resumeTasks resumes the tasks that waited for this frame, ending each
 * cancelled one. A task that waits again waits for the next frame.
 */
export function resumeTasks(): void {
  const batch = waiting
  waiting = []
  for (const waiter of batch) {
    if (waiter.task.cancelled) {
      waiter.end(new Cancelled())
    } else {
      waiter.resume()
    }
  }
}
