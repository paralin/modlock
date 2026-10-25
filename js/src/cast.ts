// Cast keeps a mode's players: one member for each human who finished
// joining, made by the mode's join and ended by its leave. Members are keyed
// by slot and connection generation, so a reconnect is a leave and a join.
// Cast learns of joins and leaves from the host's player reports and settles
// them on the next frame; it never walks the player list on a frame where
// nothing changed.
//
// A member may carry a want: to sit on a hero and team, and to come back a
// while after its hero falls. Each frame Cast compares the want with what the
// member's hero shows, asks the game again at most once a second while they
// differ, and stops asking once they match. That is a reconciliation loop: a
// lost or refused ask is only asked again, never reported as a failure.
//
// A frame costs one pawn read per member, which the mode reads back with
// pawn instead of asking the game again.

import { heroes } from './heroes.js'
import {
  type Connection,
  type Pawn,
  type Player,
  serverCommand,
} from './host.gen.js'
import { watchPlayers } from './players.js'

/** Want is what a member's hero should be; each field it leaves out is free. */
export interface Want {
  /**
   * hero seats the member on a hero: its name, such as 'hero_atlas', its id,
   * or 'any' for the hero they have or else a random one.
   */
  readonly hero?: string | number
  /**
   * team is the team, 2 or 3, to seat the member on. Without one a member
   * keeps their hero's team, or else joins the side with fewer members.
   */
  readonly team?: number
  /** respawn is the seconds a fallen hero waits before Cast asks for it back. */
  readonly respawn?: number
}

/** CastOptions are a mode's answers to its players coming and going. */
export interface CastOptions<M> {
  /** join makes the member for a human who finished joining. */
  join(connection: Connection): M
  /** leave ends a member whose connection left the slot. */
  leave?(member: M, connection: Connection): void
  /** want is what a new member wants until the mode changes it. */
  want?: Want
  /**
   * bots says what to do with a bot: skip leaves it out of the cast, join
   * makes it a member as a human is, and kick asks the server to remove it,
   * once per connection. Skip is the default.
   */
  bots?: 'skip' | 'join' | 'kick'
}

/** Seat is one member with what Cast last saw of their hero. */
interface Seat<M> {
  readonly connection: Connection
  readonly member: M
  want: Want
  /** pawn is the member's hero this frame. */
  pawn: Pawn | undefined
  alive: boolean
  /** fellAt is when Cast last saw the hero fall. */
  fellAt: number
  /** askedAt is when Cast last asked the game for the want. */
  askedAt: number
}

/** Cast keeps one member of type M per human player. */
export class Cast<M> {
  private readonly seats = new Map<number, Seat<M>>()
  /** kicked holds the connection generation each kicked bot's slot had. */
  private readonly kicked = new Map<number, number>()
  /** reported is the player list the host reported since the last frame. */
  private reported: readonly Connection[] | undefined

  constructor(private readonly options: CastOptions<M>) {
    watchPlayers((players) => {
      this.reported = players
    })
  }

  /** size is the number of members. */
  get size(): number {
    return this.seats.size
  }

  /** members lists the members in the order they joined. */
  members(): M[] {
    return Array.from(this.seats.values(), (seat) => seat.member)
  }

  /** get returns the member in player's slot, if any. */
  get(player: Player): M | undefined {
    return this.seats.get(player.slot)?.member
  }

  /** pawn returns the member's hero as Cast read it this frame. */
  pawn(player: Player): Pawn | undefined {
    return this.seats.get(player.slot)?.pawn
  }

  /** want sets what the member in player's slot should be, replacing the last want. */
  want(player: Player, want: Want): void {
    const seat = this.seats.get(player.slot)
    if (seat !== undefined) seat.want = want
  }

  /**
   * frame settles the joins and leaves reported since the last frame, reads
   * each member's hero, and asks the game once a second for any want it does
   * not show yet. seconds is the game clock.
   */
  frame(seconds: number): void {
    if (this.reported !== undefined) {
      this.settle(this.reported)
      this.reported = undefined
    }
    for (const seat of this.seats.values()) {
      this.reconcile(seat, seconds)
    }
  }

  /** settle ends the members whose connection left and makes the new ones. */
  private settle(players: readonly Connection[]): void {
    // A member leaves when their slot empties or a new connection takes it.
    const present = new Map(players.map((c) => [c.player.slot, c]))
    for (const [slot, seat] of this.seats) {
      if (present.get(slot)?.generation !== seat.connection.generation) {
        this.seats.delete(slot)
        this.options.leave?.(seat.member, seat.connection)
      }
    }

    // A human who finished joining becomes a member, and a bot as the
    // policy says.
    const { bots = 'skip' } = this.options
    for (const connection of players) {
      const slot = connection.player.slot
      if (!connection.ready || this.seats.has(slot)) continue
      if (connection.bot && bots !== 'join') {
        if (bots === 'kick') this.kick(connection)
        continue
      }
      this.seats.set(slot, {
        connection,
        member: this.options.join(connection),
        want: this.options.want ?? {},
        pawn: undefined,
        alive: false,
        fellAt: -Infinity,
        askedAt: -Infinity,
      })
    }
  }

  /** kick asks the server to remove a bot, once per connection. */
  private kick(connection: Connection): void {
    const slot = connection.player.slot
    if (this.kicked.get(slot) === connection.generation) return
    this.kicked.set(slot, connection.generation)
    serverCommand(`kick "${connection.name}"`)
  }

  /** reconcile reads a member's hero and asks for the part of the want it lacks. */
  private reconcile(seat: Seat<M>, now: number): void {
    // Note when the hero falls.
    const pawn = seat.connection.player.pawn()
    const alive = pawn !== undefined && pawn.hero !== 0 && pawn.health > 0
    if (seat.alive && !alive) seat.fellAt = now
    seat.pawn = pawn
    seat.alive = alive

    // Ask at most once a second, first for the seat, then for the hero back.
    if (now - seat.askedAt < 1) return
    const { hero, respawn } = seat.want
    if (hero !== undefined && !this.seated(seat, hero)) {
      seat.askedAt = now
      this.seat(seat, hero)
    } else if (
      respawn !== undefined &&
      pawn !== undefined &&
      pawn.hero !== 0 &&
      !alive &&
      now - seat.fellAt >= respawn
    ) {
      seat.askedAt = now
      seat.connection.player.respawn()
    }
  }

  /** seated reports whether the member's hero shows the hero and team they want. */
  private seated(seat: Seat<M>, hero: string | number): boolean {
    // A member needs a hero on the team they want before the hero counts.
    const { pawn, want } = seat
    if (pawn === undefined || pawn.hero === 0) return false
    if (want.team !== undefined && pawn.team !== want.team) return false
    if (hero === 'any') return true

    // A name outside the released heroes, such as one the game added since,
    // is met by whatever hero the game gave for it.
    const id =
      typeof hero === 'number'
        ? hero
        : heroes.find((known) => known.name === hero)?.id
    return id === undefined || pawn.hero === id
  }

  /** seat asks the game to put the member on hero and their team. */
  private seat(seat: Seat<M>, hero: string | number): void {
    // Any hero keeps the one the member has, or else takes a random one.
    const { pawn, want } = seat
    let pick: string | number | undefined = hero
    if (hero === 'any') {
      pick =
        pawn !== undefined && pawn.hero !== 0
          ? pawn.hero
          : heroes[Math.floor(Math.random() * heroes.length)]?.name
    }
    if (pick !== undefined) {
      seat.connection.player.selectHero(pick, want.team ?? this.side(pawn))
    }
  }

  /** side is the team a member joins: their hero's, or the one with fewer members. */
  private side(pawn: Pawn | undefined): number {
    // A hero on a team keeps it; otherwise count the members on each side.
    if (pawn?.team === 2 || pawn?.team === 3) return pawn.team
    let amber = 0
    let sapphire = 0
    for (const { pawn: other } of this.seats.values()) {
      if (other?.team === 2) amber++
      if (other?.team === 3) sapphire++
    }
    return sapphire < amber ? 3 : 2
  }
}
