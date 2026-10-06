// The arena's rules: who fights, the phases of a match, and the score.
import {
  addBot,
  type Connection,
  type DamagedEvent,
  type DamageEvent,
  type DamageResult,
  holdModifierState,
  log,
  type Player,
  players,
  serverCommand,
  toast,
} from 'modlock'

/** killTarget is the number of defeats that wins a match. */
export const killTarget = 10

/** matchSeconds ends a match that nobody wins first. */
const matchSeconds = 300

/** countdownSeconds holds the fighters still before a match. */
const countdownSeconds = 5

/** respawnSeconds is how long a fallen fighter waits to return. */
const respawnSeconds = 3

/** protectionSeconds shields a returning hero while it finds its feet. */
const protectionSeconds = 2

/** resultSeconds shows the result before the arena waits for the next match. */
const resultSeconds = 8

/** friendlyFire lets every hero hurt every other, whatever its team. */
const friendlyFire = 'MODIFIER_STATE_FRIENDLY_FIRE_ENABLED'

/**
 * serverConfig empties the lanes so only heroes fight. A server that
 * hibernates when empty runs no frames, so the mod could not start a match.
 */
const serverConfig = [
  'sv_hibernate_when_empty 0',
  'citadel_trooper_spawn_enabled 0',
  'citadel_neutral_spawn_enabled 0',
  'citadel_spawn_practice_bots 0',
]

/** botHero is the hero practice bots play. */
const botHero = 'hero_wraith'

/** Phase is where the match stands. */
export type Phase = 'waiting' | 'countdown' | 'live' | 'results'

/** Fighter is one connected player's place and score in the arena. */
export interface Fighter {
  player: Player
  /** generation is the connection the fighter belongs to. */
  generation: number
  name: string
  /** entered is true once the player types /ready, until /leave. */
  entered: boolean
  /** pawn is the entity of the player's hero, or zero before it spawns. */
  pawn: number
  alive: boolean
  kills: number
  deaths: number
  streak: number
  /** diedAt is when the hero last fell. */
  diedAt: number
  /** revivedAt is when the mod last asked for the hero back. */
  revivedAt: number
  /** protectedUntil blocks the hits on a returning hero until then. */
  protectedUntil: number
}

/**
 * Match runs free-for-all matches for the players in the arena: two ready
 * fighters start a countdown, the first to killTarget defeats wins, and the
 * arena waits for the next match. The mod calls it on the server's frame.
 */
export class Match {
  phase: Phase = 'waiting'
  /** deadline ends the countdown, the match or the result. */
  deadline = 0
  /** now is the game clock at the latest frame. */
  now = 0
  /** result is the last match's outcome. */
  result = ''
  /** fighters holds each connected player's fighter by slot. */
  readonly fighters = new Map<number, Fighter>()
  /** bots holds the practice bots the mod added. */
  private readonly bots: Player[] = []

  /** start configures the server for the arena. */
  start(): void {
    for (const line of serverConfig) {
      serverCommand(line)
    }
  }

  /** world forgets the previous world's fighters and bots; it removed them. */
  world(): void {
    this.fighters.clear()
    this.bots.length = 0
    this.phase = 'waiting'
  }

  /** frame follows every player's hero and advances the match. */
  frame(seconds: number): void {
    // Follow the hero of every player who has finished joining.
    this.now = seconds
    const present = new Set<number>()
    for (const connection of players()) {
      if (connection.ready) {
        present.add(connection.player.slot)
        this.follow(this.fighterOf(connection))
      }
    }

    // Forget the players who left, then step the match.
    for (const slot of this.fighters.keys()) {
      if (!present.has(slot)) {
        this.fighters.delete(slot)
      }
    }
    this.advance()
  }

  /**
   * damage blocks every hit on a fighter outside a live match and on a hero
   * that has just returned. Other hits land unchanged.
   */
  damage(hit: DamageEvent): DamageResult | void {
    const victim = this.fighterAt(hit.victim)
    if (
      victim?.entered &&
      (this.phase !== 'live' || this.now < victim.protectedUntil)
    ) {
      return { block: true }
    }
  }

  /** damaged scores each hit that defeated a fighter during a live match. */
  damaged(hit: DamagedEvent): void {
    // Only a lethal hit on a fighter in a live match counts.
    const victim = this.fighterAt(hit.victim)
    if (
      this.phase !== 'live' ||
      !victim?.entered ||
      hit.healthBefore <= 0 ||
      hit.healthLost < hit.healthBefore
    ) {
      return
    }
    victim.deaths++
    victim.streak = 0

    // Credit another fighter who landed it.
    const attacker = this.fighterAt(hit.attacker)
    if (!attacker?.entered || attacker === victim) {
      this.tell(`${victim.name} falls.`)
      return
    }
    attacker.kills++
    attacker.streak++
    attacker.player.addMetric('arena.kills', 1)
    attacker.player.addMetric('arena.streak', attacker.streak)

    // Tell everyone, and end the match on the winning defeat.
    const streak = attacker.streak >= 3 ? `, ${attacker.streak} in a row` : ''
    this.tell(`${attacker.name} defeats ${victim.name}${streak}.`)
    if (attacker.kills >= killTarget) {
      this.finish(attacker)
    }
  }

  /** ready enters player in the current or the next match. */
  ready(player: Player): void {
    // Only a player in the arena can enter, and only once.
    const fighter = this.fighters.get(player.slot)
    if (fighter === undefined) {
      return
    }
    if (fighter.entered) {
      toast(player, 'You are already in.')
      return
    }

    // Hold a fighter who enters during the countdown with the others.
    fighter.entered = true
    fighter.player.freeze(this.phase === 'countdown')
    toast(player, this.phase === 'live' ? 'You join the fight.' : 'Ready.')
  }

  /** leave withdraws player to watch. */
  leave(player: Player): void {
    const fighter = this.fighters.get(player.slot)
    if (fighter?.entered) {
      fighter.entered = false
      fighter.player.freeze(false)
      toast(player, 'You are watching. Type /ready to fight again.')
    }
  }

  /** addBot adds a practice bot that enters at once, and says how it went. */
  addBot(): string {
    // Alternate the bots between the two teams.
    const name = `Arena Bot ${this.bots.length + 1}`
    const team = 2 + (this.bots.length % 2)
    const bot = addBot({ name, team, hero: botHero })
    if (bot === undefined) {
      return 'The bot could not join.'
    }

    // The bot enters once its connection shows up in the next frame.
    this.bots.push(bot)
    this.fighters.set(bot.slot, this.fresh(bot, -1, name, true))
    return `${name} joins the arena.`
  }

  /** removeBots removes every practice bot. */
  removeBots(): string {
    for (const bot of this.bots) {
      bot.removeBot()
      this.fighters.delete(bot.slot)
    }
    const count = this.bots.length
    this.bots.length = 0
    return count === 0 ? 'There are no bots.' : 'The bots leave.'
  }

  /** entrants lists the fighters who typed /ready. */
  entrants(): Fighter[] {
    return [...this.fighters.values()].filter((fighter) => fighter.entered)
  }

  /** fighterAt returns the fighter whose hero is the entity pawn. */
  private fighterAt(pawn: number): Fighter | undefined {
    if (pawn === 0) {
      return undefined
    }
    return [...this.fighters.values()].find((fighter) => fighter.pawn === pawn)
  }

  /**
   * fighterOf returns the fighter of connection, starting a fresh one when
   * someone new takes the slot.
   */
  private fighterOf(connection: Connection): Fighter {
    // A bot's fighter was made when it was added; adopt its connection.
    const { player } = connection
    let fighter = this.fighters.get(player.slot)
    if (fighter?.generation === -1) {
      fighter.generation = connection.generation
    }

    // Anyone else new to the slot starts fresh, and a person hears the rules.
    if (fighter === undefined || fighter.generation !== connection.generation) {
      fighter = this.fresh(
        player,
        connection.generation,
        connection.name,
        false,
      )
      this.fighters.set(player.slot, fighter)
      if (!connection.bot) {
        player.announce(
          'ARENA',
          `Every hero is an opponent. First to ${killTarget} defeats wins. Type /ready to fight.`,
        )
      }
    }
    fighter.name = connection.name
    return fighter
  }

  /** fresh returns a new fighter for player, entered at once when asked. */
  private fresh(
    player: Player,
    generation: number,
    name: string,
    entered: boolean,
  ): Fighter {
    return {
      player,
      generation,
      name,
      entered,
      pawn: 0,
      alive: false,
      kills: 0,
      deaths: 0,
      streak: 0,
      diedAt: 0,
      revivedAt: 0,
      protectedUntil: 0,
    }
  }

  /**
   * follow notes whether fighter's hero lives, shields it as it returns, and
   * brings a fallen hero back after the respawn delay.
   */
  private follow(fighter: Fighter): void {
    // A player without a hero has nothing to follow yet.
    const pawn = fighter.player.pawn()
    if (pawn === undefined) {
      fighter.alive = false
      return
    }

    // A hero that appears or returns can hurt every other hero, and a
    // returning one is shielded for a moment.
    const alive = pawn.health > 0
    if (alive && (!fighter.alive || pawn.entity !== fighter.pawn)) {
      holdModifierState(pawn.entity, friendlyFire)
      fighter.protectedUntil = this.now + protectionSeconds
    }
    if (!alive && fighter.alive) {
      fighter.diedAt = this.now
    }
    fighter.pawn = pawn.entity
    fighter.alive = alive

    // Ask for a fallen hero back once a second after the delay, until it
    // returns.
    const due = this.now - fighter.diedAt >= respawnSeconds
    if (!alive && due && this.now - fighter.revivedAt >= 1) {
      fighter.revivedAt = this.now
      fighter.player.respawn()
    }
  }

  /** advance moves the match through its phases on the game clock. */
  private advance(): void {
    const standing = this.entrants().filter((fighter) => fighter.alive)
    switch (this.phase) {
      case 'waiting':
        if (standing.length >= 2) {
          this.countdown()
        }
        return
      case 'countdown':
        if (standing.length < 2) {
          this.phase = 'waiting'
          this.freeze(false)
        } else if (this.now >= this.deadline) {
          this.begin()
        }
        return
      case 'live':
        if (this.now >= this.deadline || this.entrants().length < 2) {
          this.finish(this.leader())
        }
        return
      case 'results':
        if (this.now >= this.deadline) {
          this.phase = 'waiting'
        }
    }
  }

  /** countdown clears the scores and holds the fighters still. */
  private countdown(): void {
    // Start the clock with every score at zero.
    this.phase = 'countdown'
    this.deadline = this.now + countdownSeconds
    for (const fighter of this.fighters.values()) {
      Object.assign(fighter, { kills: 0, deaths: 0, streak: 0 })
    }

    // Hold the fighters still and tell everyone.
    this.freeze(true)
    this.announce('GET READY', `First to ${killTarget} defeats wins.`)
  }

  /** begin releases the fighters into a live match. */
  private begin(): void {
    // Start the match clock and release the fighters.
    this.phase = 'live'
    this.deadline = this.now + matchSeconds
    this.freeze(false)

    // Tell everyone the match is on.
    this.announce('FIGHT', 'Every hero is an opponent.')
    log(`match started with ${this.entrants().length} fighters`)
  }

  /** finish ends the match with winner, or a draw without one. */
  private finish(winner: Fighter | undefined): void {
    // Hold the result on screen until the arena waits again.
    this.phase = 'results'
    this.deadline = this.now + resultSeconds
    this.result = winner
      ? `${winner.name} wins with ${winner.kills} defeats`
      : 'The match is a draw'

    // Credit the winner and tell everyone.
    winner?.player.addMetric('arena.wins', 1)
    this.announce('MATCH OVER', this.result)
    log(`match over: ${this.result}`)
  }

  /** leader returns the fighter with the most defeats, unless two share it. */
  private leader(): Fighter | undefined {
    const [first, second] = this.entrants().sort((a, b) => b.kills - a.kills)
    return first && first.kills > (second?.kills ?? -1) ? first : undefined
  }

  /** freeze holds or releases every entrant's hero. */
  private freeze(frozen: boolean): void {
    for (const fighter of this.entrants()) {
      fighter.player.freeze(frozen)
    }
  }

  /** announce shows a title and a line to every player. */
  private announce(title: string, text: string): void {
    for (const fighter of this.fighters.values()) {
      fighter.player.announce(title, text)
    }
  }

  /** tell shows every player a short notice. */
  private tell(text: string): void {
    for (const fighter of this.fighters.values()) {
      toast(fighter.player, text)
    }
  }
}
