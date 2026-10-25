// frameClock times a mod's frames and the systems inside them, so a slow
// server's log shows how much of its frame is the mod's and which system
// spends it. Each system calls lap once it finishes its part of the frame.
// A minute's slowest frame is named by its own laps, since a system that
// spikes once a minute ranks far below the steady ones by mean.
//
//   import { frameClock, onFrame } from 'modlock'
//
//   onFrame((frame) => {
//     frameClock.begin()
//     moveBots()
//     frameClock.lap('bots')
//     drawHud()
//     frameClock.lap('hud')
//     frameClock.end(frame.tick)
//   })

import { log } from './host.gen.js'

/** now reads the runtime's millisecond clock. */
const now = () => performance.now()

/** reportTicks is a minute of ticks, the period of each report. */
const reportTicks = 3840n

/** reportedSystems is how many of the costliest systems a report names. */
const reportedSystems = 8

/** reportedLaps is how many laps of the slowest frame a report names. */
const reportedLaps = 3

/** Cost is one system's time over the current minute. */
interface Cost {
  total: number
  max: number
}

/** FrameClock times the frames of one mod. */
class FrameClock {
  /** frames holds the milliseconds each frame of the current minute took. */
  private readonly frames: number[] = []
  private readonly costs = new Map<string, Cost>()
  /** laps holds the current frame's systems and milliseconds. */
  private laps: [string, number][] = []
  /** slowest holds the laps of the minute's slowest frame. */
  private slowest: [string, number][] = []
  /** slowestMs is the slowest frame's milliseconds, below any frame when none ran. */
  private slowestMs = -1
  private start = 0
  private mark = 0
  /** reportAt is the tick that ends the current minute. */
  private reportAt = 0n

  /** begin starts timing a frame. */
  begin(): void {
    this.start = now()
    this.mark = this.start
    this.laps.length = 0
  }

  /** elapsed is the milliseconds since the frame began. */
  get elapsed(): number {
    return now() - this.start
  }

  /** lap charges the time since the previous lap, or the frame's start, to system. */
  lap(system: string): void {
    // Measure from the previous lap.
    const time = now()
    const spent = time - this.mark
    this.mark = time
    this.laps.push([system, spent])

    // Add it to the system's minute.
    const cost = this.costs.get(system)
    if (cost) {
      cost.total += spent
      cost.max = Math.max(cost.max, spent)
    } else {
      this.costs.set(system, { total: spent, max: spent })
    }
  }

  /**
   * end finishes the frame and, once a minute of ticks passed, logs the
   * minute's median and slowest frame with the systems that cost the most:
   * each one's mean and slowest milliseconds a frame, and the slowest frame's
   * costliest laps, to the server log.
   * The minute ends on the first frame at or past its last tick, since the
   * host skips a slow mod's frames and may skip that tick.
   */
  end(tick: bigint): void {
    // Collect the frame, keeping the slowest one's laps, until the minute
    // ends. The arrays swap rather than copy.
    const elapsed = this.elapsed
    this.frames.push(elapsed)
    if (elapsed > this.slowestMs) {
      this.slowestMs = elapsed
      ;[this.slowest, this.laps] = [this.laps, this.slowest]
    }
    if (this.reportAt === 0n) {
      this.reportAt = tick + reportTicks
    }
    if (tick < this.reportAt) {
      return
    }
    this.reportAt = tick + reportTicks

    // Log the minute's frames and costliest systems, and start the next.
    const frames = this.frames.sort((a, b) => a - b)
    const p50 = frames.at(frames.length >> 1) ?? 0
    const systems = [...this.costs]
      .sort(([, a], [, b]) => b.total - a.total)
      .slice(0, reportedSystems)
      .map(
        ([system, { total, max }]) =>
          `${system} ${(total / frames.length).toFixed(2)}/${max.toFixed(1)}`,
      )
    const laps = this.slowest
      .sort(([, a], [, b]) => b - a)
      .slice(0, reportedLaps)
      .map(([system, spent]) => `${system} ${spent.toFixed(1)}`)
    log(
      `frame work: p50 ${p50.toFixed(1)} max ${frames.at(-1)!.toFixed(1)} ms over ${frames.length} frames; mean/max ms: ${systems.join(', ')}; slowest frame ms: ${laps.join(', ')}`,
    )
    this.frames.length = 0
    this.costs.clear()
    this.slowestMs = -1
  }
}

/** frameClock is the mod's one frame clock. */
export const frameClock = new FrameClock()
