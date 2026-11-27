// Timelines run one mod's timed presentations. Each timeline is in one of the
// states of Timeline.tla: idle, running, held while a game-clock timeline
// waits out the game's pause, or done, which keeps its last looks. Every step
// whose time has come applies at once, in time order, so a late callback
// catches up rather than drifting, and a callback from an earlier run, a
// drop or a pause finds its token stale and does nothing.

import {
  TimelineClock,
  type Style,
  type Timeline,
  type TimelineStep,
} from '../../src/wasm/ui/ui.pb.js'

/**
 * Look is what a timeline gives the nodes of one measure: classes and a style
 * over their own.
 */
export interface Look {
  readonly classes: readonly string[]
  readonly style: Style | undefined
}

/** TimelineHost is what the timelines reach the game through. */
export interface TimelineHost {
  /** schedule runs callback after seconds of the player's time. */
  schedule(seconds: number, callback: () => void): void
  /** now is the player's time in seconds. */
  now(): number
  /** look tells the drawing that the looks of the nodes measure names changed. */
  look(measure: string): void
  /** play plays a sound, cut after seconds unless seconds is zero. */
  play(sound: string, seconds: number): void
}

/** maxAt bounds a step's time, as the host does. */
const maxAt = 60

/** Run is one timeline's current run. */
interface Run {
  run: number
  clock: TimelineClock
  /** steps are the run's steps in time order. */
  steps: TimelineStep[]
  /** next is the first step not yet applied. */
  next: number
  /** elapsed is the clock's seconds before since. */
  elapsed: number
  /** since is when the clock last started running, or null while held. */
  since: number | null
  /**
   * token outdates callbacks scheduled before the last change: a later
   * schedule, a pause or a drop.
   */
  token: number
  /** looks holds the look each measure has from this run. */
  looks: Map<string, Look>
}

/** Timelines holds one mod's timelines. */
export class Timelines {
  private readonly runs = new Map<string, Run>()
  private paused = false

  /**
   * joining makes the first draw join every run at its end, for a renderer
   * that meets the mod's lifetime part way through.
   */
  constructor(
    private readonly host: TimelineHost,
    private joining: boolean,
  ) {}

  /**
   * draw takes the root's timelines. A joining first call shows every run's
   * last looks; other calls start a timeline new to them or whose run rose,
   * and clear one dropped, which is how a mod cancels it.
   */
  draw(timelines: readonly Timeline[]): void {
    // Start each timeline new to this draw or whose run rose, skipping a
    // repeated id.
    const joining = this.joining
    this.joining = false
    const ids = new Set<string>()
    for (const each of timelines) {
      const id = each.id ?? ''
      if (ids.has(id)) {
        continue
      }
      ids.add(id)
      const old = this.runs.get(id)
      const run = each.run ?? 0
      if (old?.run === run) {
        continue
      }
      if (old) {
        this.clear(old)
      }
      const fresh = this.start(each)
      this.runs.set(id, fresh)
      if (joining) {
        this.apply(fresh, Infinity, false)
      } else {
        this.advance(fresh)
      }
    }

    // Clear the timelines the root dropped.
    for (const [id, old] of this.runs) {
      if (!ids.has(id)) {
        this.clear(old)
        this.runs.delete(id)
      }
    }
  }

  /** pause holds every game-clock run while paused, and runs them after. */
  pause(paused: boolean): void {
    if (paused === this.paused) {
      return
    }
    this.paused = paused
    const now = this.host.now()
    for (const run of this.runs.values()) {
      if (run.clock !== TimelineClock.GAME || this.done(run)) {
        continue
      }
      run.token++
      if (paused && run.since !== null) {
        run.elapsed += now - run.since
        run.since = null
      } else if (!paused && run.since === null) {
        run.since = now
        this.schedule(run)
      }
    }
  }

  /** state names a timeline's state in Timeline.tla's terms. */
  state(id: string): 'idle' | 'running' | 'held' | 'done' {
    const run = this.runs.get(id)
    if (!run) {
      return 'idle'
    }
    if (this.done(run)) {
      return 'done'
    }
    return run.since === null ? 'held' : 'running'
  }

  /**
   * look returns measure's look from every timeline, a later id's style on
   * top.
   */
  look(measure: string): Look | null {
    const looks = [...this.runs.values()]
      .map((run) => run.looks.get(measure))
      .filter((each): each is Look => each !== undefined)
    if (looks.length === 0) {
      return null
    }
    return {
      classes: [...new Set(looks.flatMap((each) => each.classes))],
      style: looks.reduce<Style>(
        (style, each) => ({ ...style, ...each.style }),
        {},
      ),
    }
  }

  /** release clears every timeline, as the mod's interface goes. */
  release(): void {
    for (const run of this.runs.values()) {
      this.clear(run)
    }
    this.runs.clear()
  }

  /** start makes a run of timeline at its start, held if it must be. */
  private start(timeline: Timeline): Run {
    const clock = timeline.clock ?? TimelineClock.WALL
    const held = clock === TimelineClock.GAME && this.paused
    return {
      run: timeline.run ?? 0,
      clock,
      steps: [...(timeline.steps ?? [])].sort((a, b) => at(a) - at(b)),
      next: 0,
      elapsed: 0,
      since: held ? null : this.host.now(),
      token: 0,
      looks: new Map(),
    }
  }

  /** advance applies the steps whose time has come, then waits for the next. */
  private advance(run: Run): void {
    if (run.since === null) {
      return
    }
    this.apply(run, run.elapsed + this.host.now() - run.since, true)
    this.schedule(run)
  }

  /** schedule wakes the run at its next step, if it has one. */
  private schedule(run: Run): void {
    // Wait for the next step, outdating any earlier wait.
    const step = run.steps[run.next]
    if (step === undefined || run.since === null) {
      return
    }
    const token = ++run.token
    const wait = at(step) - run.elapsed - (this.host.now() - run.since)
    this.host.schedule(Math.max(0, wait), () => {
      if (run.token === token) {
        this.advance(run)
      }
    })
  }

  /** apply applies run's steps up to time, playing their sounds if sounding. */
  private apply(run: Run, time: number, sounding: boolean): void {
    const changed = new Set<string>()
    while (run.next < run.steps.length && at(run.steps[run.next]!) <= time) {
      const step = run.steps[run.next++]!
      const measure = step.measure ?? ''
      if (measure !== '') {
        run.looks.set(measure, {
          classes: step.classes ?? [],
          style: step.style,
        })
        changed.add(measure)
      }
      if (sounding && step.sound) {
        this.host.play(step.sound, step.seconds ?? 0)
      }
    }
    for (const measure of changed) {
      this.host.look(measure)
    }
  }

  /** clear outdates run's callbacks and takes back its looks. */
  private clear(run: Run): void {
    // Outdate the run's waits, then tell the drawing which looks went.
    run.token++
    const measures = [...run.looks.keys()]
    run.looks.clear()
    run.next = run.steps.length
    for (const measure of measures) {
      this.host.look(measure)
    }
  }

  private done(run: Run): boolean {
    return run.next >= run.steps.length
  }
}

/** at reads a step's time, bounded as the host bounds it. */
function at(step: TimelineStep): number {
  return Math.min(Math.max(step.at ?? 0, 0), maxAt)
}
