// A drill is one player's timed run against target heroes that stand still
// in front of them and hop to a new spot each time they are hit.
import {
  addBot,
  type Angles,
  type DamageEvent,
  type Player,
  type Vector,
} from 'modlock'

/** targetCount is how many targets stand in a drill. */
const targetCount = 3

/** targetHero is the hero every target plays. */
const targetHero = 'hero_wraith'

/** headGroup is the hit group of a shot to the head. */
const headGroup = 1

/** arcDegrees is how far to either side of the shooter's aim a target stands. */
const arcDegrees = 35

/** near and far bound a target's distance from where the drill started. */
const near = 500
const far = 900

/**
 * serverConfig keeps the server running frames while empty and keeps every
 * target hero still and harmless.
 */
export const serverConfig = [
  'sv_hibernate_when_empty 0',
  'citadel_bot_test_mode 1',
  'citadel_bot_brain_disable_movement 1',
  'citadel_bot_brain_disable_attacks 1',
]

/** Target is one target hero and the spot it should stand on. */
interface Target {
  bot: Player
  spot: Vector
  /** entity is the target's hero once it stands on its spot, or zero. */
  entity: number
}

/** Drill is one shooter's run: the targets, the score and the deadline. */
export class Drill {
  score = 0
  headshots = 0
  private readonly targets: Target[] = []

  constructor(
    readonly shooter: Player,
    readonly endsAt: number,
    private readonly origin: Vector,
    private readonly yaw: number,
  ) {}

  /** start adds the targets to the team against team, and reports whether they joined. */
  start(team: number): boolean {
    for (let index = 0; index < targetCount; index++) {
      const spot = this.spot()
      const bot = addBot({
        name: 'Target',
        team: team === 2 ? 3 : 2,
        hero: targetHero,
        position: spot,
      })
      if (!bot) {
        this.stop()
        return false
      }
      this.targets.push({ bot, spot, entity: 0 })
    }
    return true
  }

  /** frame places each target whose hero has arrived since it joined. */
  frame(): void {
    for (const target of this.targets) {
      if (target.entity === 0) {
        this.place(target)
      }
    }
  }

  /**
   * hit scores the shooter's shot on one of the drill's targets and moves
   * that target. It reports whether the victim is one of the targets.
   */
  hit(hit: DamageEvent): boolean {
    // Only the shooter's own shots on a target score.
    const target = this.targets.find(({ entity }) => entity === hit.victim)
    if (!target) {
      return false
    }
    if (hit.attacker !== this.shooter.pawn()?.entity) {
      return true
    }

    // A shot to the head scores double and rings.
    const head = hit.hitGroup === headGroup
    this.score += head ? 2 : 1
    if (head) {
      this.headshots++
      this.shooter.sound('Damage.Send.Crit')
    }
    target.spot = this.spot()
    this.place(target)
    return true
  }

  /** stop removes the targets. */
  stop(): void {
    for (const { bot } of this.targets) {
      bot.removeBot()
    }
    this.targets.length = 0
  }

  /** place moves a target's hero onto its spot, facing the shooter. */
  private place(target: Target): void {
    const pawn = target.bot.pawn()
    if (pawn && target.bot.teleport(target.spot, this.facing())) {
      target.entity = pawn.entity
    }
  }

  /** spot picks a random place in the arc in front of the shooter. */
  private spot(): Vector {
    const yaw = this.yaw + (Math.random() * 2 - 1) * arcDegrees
    const distance = near + Math.random() * (far - near)
    const radians = (yaw * Math.PI) / 180
    return {
      x: this.origin.x + Math.cos(radians) * distance,
      y: this.origin.y + Math.sin(radians) * distance,
      z: this.origin.z,
    }
  }

  /** facing turns a target back toward where the drill started. */
  private facing(): Angles {
    return { pitch: 0, yaw: this.yaw + 180, roll: 0 }
  }
}
