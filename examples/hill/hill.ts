// The hill: a ring of markers in the world, and each team's hold on it.
import {
  type Connection,
  createText,
  type Player,
  players,
  type Vector,
  type WorldObject,
} from 'modlock'

/** radius is how far from the center a hero still stands on the hill. */
const radius = 300

/** reach is how far above or below the center a hero still counts. */
const reach = 200

/** markers is the number of markers in the ring. */
const markers = 16

/** winScore is the score that wins a round; a team scores once a second. */
export const winScore = 60

/** breakSeconds is the pause between a won round and the next. */
const breakSeconds = 10

/** Team is one side's name, color and score. */
export interface Team {
  readonly id: number
  readonly name: string
  /** color is '#rrggbb' for the HUD. */
  readonly color: string
  score: number
}

/** Hold is who holds the hill this second. */
export type Hold = 'nobody' | 'contested' | Team

/**
 * Hill runs rounds on a hill a player raises with /hill: each second, a team
 * alone on it scores a point, and the first to winScore wins the round.
 */
export class Hill {
  /** teams are Deadlock's two sides, by team number. */
  readonly teams: readonly Team[] = [
    { id: 2, name: 'Amber', color: '#ffb030', score: 0 },
    { id: 3, name: 'Sapphire', color: '#4a90ff', score: 0 },
  ]
  hold: Hold = 'nobody'
  /** winner is the team that won the round, until the next one starts. */
  winner?: Team
  center?: Vector
  private ring: WorldObject[] = []
  private sign?: WorldObject
  /** nextSecond is the game clock of the next scoring second. */
  private nextSecond = 0
  /** nextRound starts the next round after a win. */
  private nextRound = 0

  /** raise moves the hill to position and starts a fresh round. */
  raise(position: Vector): boolean {
    // Lay a ring of markers on the ground around the center.
    this.remove()
    for (let i = 0; i < markers; i++) {
      const angle = (i / markers) * 2 * Math.PI
      const marker = createText({
        text: '◆',
        position: {
          x: position.x + Math.cos(angle) * radius,
          y: position.y + Math.sin(angle) * radius,
          z: position.z + 16,
        },
        fontSize: 48,
        color: 0xffffffff,
        faceCamera: true,
      })
      if (marker) {
        this.ring.push(marker)
      }
    }

    // Float a sign over the center and start the round.
    this.sign = createText({
      text: 'THE HILL',
      position: { ...position, z: position.z + 160 },
      fontSize: 96,
      color: 0xffd34dff,
      faceCamera: true,
    })
    this.center = position
    this.reset()
    return this.ring.length > 0
  }

  /** forget drops the hill; a new world has already removed its markers. */
  forget(): void {
    this.ring = []
    this.sign = undefined
    this.center = undefined
  }

  /** frame scores the hill once a second at the game clock now. */
  frame(now: number): void {
    // Score only on a raised hill, between breaks, once a second.
    if (this.center === undefined) {
      return
    }
    if (this.winner && now >= this.nextRound) {
      this.reset()
    }
    if (this.winner || now < this.nextSecond) {
      return
    }
    this.nextSecond = now + 1

    // A team alone on the hill scores, and each of its heroes there is
    // credited with the second.
    const standing = this.standing()
    const [team, rival] = new Set(standing.map(({ team }) => team))
    this.hold = rival ? 'contested' : (team ?? 'nobody')
    if (typeof this.hold === 'object') {
      this.hold.score++
      for (const { player } of standing) {
        player.addMetric('hill.seconds', 1)
      }
    }

    // Say who holds the hill, and end the round on the winning point.
    this.sign?.setText(this.label())
    if (typeof this.hold === 'object' && this.hold.score >= winScore) {
      this.win(this.hold, now)
    }
  }

  /** standing lists the living heroes on the hill with their teams. */
  private standing(): { player: Player; team: Team }[] {
    return players().flatMap((connection: Connection) => {
      const pawn = connection.player.pawn()
      const team = this.teams.find((t) => t.id === pawn?.team)
      if (!pawn || pawn.health <= 0 || !team || !this.covers(pawn.position)) {
        return []
      }
      return [{ player: connection.player, team }]
    })
  }

  /** covers reports whether position stands on the hill. */
  private covers(position: Vector): boolean {
    const { x, y, z } = this.center!
    return (
      Math.hypot(position.x - x, position.y - y) <= radius &&
      Math.abs(position.z - z) <= reach
    )
  }

  /** label is the sign's text for the current hold. */
  private label(): string {
    if (this.hold === 'nobody') {
      return 'THE HILL'
    }
    if (this.hold === 'contested') {
      return 'CONTESTED'
    }
    return this.hold.name.toUpperCase()
  }

  /** win ends the round for team and tells every player. */
  private win(team: Team, now: number): void {
    this.winner = team
    this.nextRound = now + breakSeconds
    for (const connection of players()) {
      connection.player.announce(
        `${team.name.toUpperCase()} WINS`,
        `${team.name} held the hill for ${winScore} seconds.`,
      )
    }
  }

  /** reset starts a fresh round. */
  private reset(): void {
    for (const team of this.teams) {
      team.score = 0
    }
    this.winner = undefined
    this.hold = 'nobody'
    this.sign?.setText('THE HILL')
  }

  /** remove takes the hill's markers out of the world. */
  private remove(): void {
    for (const marker of this.ring) {
      marker.remove()
    }
    this.sign?.remove()
    this.forget()
  }
}
