// A probe is a mod's self-check: a run on a real server that waits for each
// step of the behavior it checks, then writes one verdict line and quits the
// server, so a script can start the server and read whether the mod works.
//
//   const run = new ProbeRun('Duel', 120)
//   onFrame((frame) => {
//     run.tick(frame.timeSeconds, 'two fighters', () => fighters() >= 2)
//   })
//
// The server log then reads "probe: waiting for two fighters" once, and
// "Duel probe PASS" or "probe failed: ..." and "Duel probe FAIL" at the end.

import { log, serverCommand } from './host.gen.js'

/**
 * Check returns true when a probe passes, a failure message when it fails, or
 * false to check again next frame.
 */
export type Check = () => boolean | string

/**
 * ProbeRun drives one probe to a verdict. It times the run from its first
 * frame, logs each step the probe waits on once, fails a run that outlasts its
 * timeout, and reports PASS or FAIL before quitting the server. A frame costs
 * the check alone; the run makes host calls only when the step changes and at
 * the verdict.
 */
export class ProbeRun {
  private started?: number
  private finished = false
  private logged?: string

  /**
   * @param name is the name the verdict line reports, such as the mod's.
   * @param timeout bounds the run in seconds.
   */
  constructor(
    private readonly name: string,
    private readonly timeout: number,
  ) {}

  /** elapsed returns the seconds since the run's first frame, starting it at now. */
  elapsed(now: number): number {
    this.started ??= now
    return now - this.started
  }

  /**
   * tick runs check for the frame at now until the run ends. step names what
   * the probe waits on, and describe adds the probe's state to a timeout.
   */
  tick(now: number, step: string, check: Check, describe?: () => string): void {
    // Check the current step until it resolves or times out.
    if (this.finished) {
      return
    }
    let result: boolean | string
    if (this.elapsed(now) > this.timeout) {
      const state = describe ? `: ${describe()}` : ''
      result = `timed out waiting for ${step}${state}`
    } else {
      result = check()
    }
    const failure = typeof result === 'string' ? result : undefined
    if (failure === undefined && !result) {
      if (step !== this.logged) {
        this.logged = step
        log(`probe: waiting for ${step}`)
      }
      return
    }

    // Report the verdict and quit the server.
    this.finished = true
    if (failure !== undefined) {
      log(`probe failed: ${failure}`)
    }
    log(`${this.name} probe ${failure === undefined ? 'PASS' : 'FAIL'}`)
    serverCommand('quit')
  }
}
