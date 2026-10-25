// The arena's self-check: started with the argument probe, the mod seats two
// bots, waits for their match to go live, defeats one, and checks that it
// returns. It then quits the server with PASS or FAIL.
import { log, ProbeRun } from 'modlock'

import type { Match } from './match'

/** Step is where the check stands. */
type Step = 'seat' | 'live' | 'fall' | 'return'

/** waits names what the check waits on at each step, for the server log. */
const waits: Record<Step, string> = {
  seat: 'two bots',
  live: 'the match to go live',
  fall: 'the first bot to fall',
  return: 'the fallen bot to return',
}

/** Probe drives the check one frame at a time. */
export class Probe {
  private readonly run = new ProbeRun('Arena', 90)
  private step: Step = 'seat'
  private since = 0

  /** tick advances the check at the frame's game clock. */
  tick(match: Match, now: number): void {
    this.run.tick(now, waits[this.step], () => this.advance(match, now))
  }

  /** advance takes the next step once the arena is ready for it. */
  private advance(match: Match, now: number): boolean | string {
    switch (this.step) {
      case 'seat':
        log(match.addBot(), match.addBot())
        this.step = 'live'
        return false
      case 'live':
        if (match.phase === 'live') {
          this.since = now
          this.step = 'fall'
        }
        return false
      case 'fall':
        return this.fall(match, now)
      case 'return':
        return now - this.since > 1 && match.entrants()[0]?.alive === true
    }
  }

  /** fall defeats the first bot once its protection has passed. */
  private fall(match: Match, now: number): boolean | string {
    const first = match.entrants()[0]
    if (first === undefined) {
      return 'the bots left the arena'
    }
    if (now - this.since >= 3 && first.player.kill()) {
      this.since = now
      this.step = 'return'
    }
    return false
  }
}
