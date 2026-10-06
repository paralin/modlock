// The arena's self-check: started with the argument probe, the mod seats two
// bots, waits for their match to go live, defeats one, and checks that it
// returns. It then quits the server with PASS or FAIL.
import { log, serverCommand } from 'modlock'

import type { Match } from './match'

/** timeout bounds the whole check in seconds. */
const timeout = 90

/** Step is where the check stands. */
type Step = 'seat' | 'live' | 'fall' | 'return' | 'done'

/** Probe drives the check one frame at a time. */
export class Probe {
  private step: Step = 'seat'
  private started?: number
  private since = 0

  /** tick advances the check at the frame's game clock. */
  tick(match: Match, now: number): void {
    // Give up once the check has run too long.
    this.started ??= now
    if (this.step === 'done') {
      return
    }
    if (now - this.started > timeout) {
      this.finish(`timed out at step ${this.step}`)
      return
    }

    // Take the next step once the arena is ready for it.
    switch (this.step) {
      case 'seat':
        log(match.addBot(), match.addBot())
        this.step = 'live'
        return
      case 'live':
        if (match.phase === 'live') {
          this.since = now
          this.step = 'fall'
        }
        return
      case 'fall':
        this.fall(match, now)
        return
      case 'return':
        if (now - this.since > 1 && match.entrants()[0]?.alive) {
          this.finish()
        }
    }
  }

  /** fall defeats the first bot once its protection has passed. */
  private fall(match: Match, now: number): void {
    const first = match.entrants()[0]
    if (first === undefined) {
      this.finish('the bots left the arena')
      return
    }
    if (now - this.since >= 3 && first.player.kill()) {
      this.since = now
      this.step = 'return'
    }
  }

  /** finish logs the verdict and quits the server. */
  private finish(failure?: string): void {
    this.step = 'done'
    if (failure) {
      log(`probe failed: ${failure}`)
    }
    log(`Arena probe ${failure ? 'FAIL' : 'PASS'}`)
    serverCommand('quit')
  }
}
