// The arena's rules: who fights, the phases of a match, and the score.
import {
  addBot,
  Cast,
  type Connection,
  type DamagedEvent,
  type DamageEvent,
  type DamageResult,
  holdModifierState,
  log,
  type Player,
  toast,
} from 'modlock'
import { apply, arena } from 'modlock/console'

/** killTarget is the number of defeats that wins a match. */
export const killTarget = 10

/** matchSeconds is how long a match lasts when nobody reaches killTarget. */
const matchSeconds = 300

/** countdownSeconds is how long the fighters are held still before a match. */
const countdownSeconds = 5

/** respawnSeconds is how long a fallen fighter waits to return. */
const respawnSeconds = 3

/** protectionSeconds is how long the mod blocks hits on a returning hero. */
const protectionSeconds = 2

/** resultSeconds is how long the result stays on screen after a match. */
const resultSeconds = 8

/** friendlyFire is the modifier state that lets a hero hurt its teammates. */
const friendlyFire = 'MODIFIER_STATE_FRIENDLY_FIRE_ENABLED'

/** botHero is the hero practice bots play. */
const botHero = 'hero_wraith'

/** Phase is where the match stands. */
export type Phase = 'waiting' | 'countdown' | 'live' | 'results'

/** Fighter is one connected player's place and score in the arena. */
export interface Fighter {
  player: Player
  name: string
  /** entered is true once the player types /ready, until /leave. */
  entered: boolean
  /** pawn is the entity of the player's hero, or zero before it spawns. */
  pawn: number
  alive: boolean
  kills: number
  deaths: number
  streak: number
  /** protectedUntil is when the mod stops blocking hits on a returning hero. */
  protectedUntil: number
}

/**
 * Match keeps the fighters and moves the arena's free-for-all matches through
 * their phases: when two fighters are ready a countdown starts, the first to
 * killTarget defeats wins, and the arena then waits for the next match. The
 * mod calls frame on every server frame.
 */
export class Match {
  phase: Phase = 'waiting'
  /** deadline is when the countdown, the match or the result ends. */
  deadline = 0
  /** now is the game clock at the latest frame. */
  now = 0
  /** result is the last match's outcome. */
  result = ''
  /**
   * fighters holds a fighter for each player and practice bot, which come
   * back respawnSeconds after they fall.
   */
  readonly fighters = new Cast<Fighter>({
    join: (connection) => this.join(connection),
    want: { respawn: respawnSeconds },
    bots: 'join',
  })
  /** bots holds the practice bots the mod added. */
  private readonly bots: Player[] = []

  /**
   * start turns off troopers, jungle camps and the server's own bots so only
   * heroes fight. The preset also keeps the server running while it is empty,
   * so the mod can start a match.
   */
  start(): void {
    apply(arena)
  }

  /**
   * world forgets the practice bots and waits for a new match, because
   * loading a new map has already removed the bots.
   */
  world(): void {
    this.bots.length = 0
    this.phase = 'waiting'
  }

  /** frame follows every player's hero and advances the match. */
  frame(seconds: number): void {
    // Follow the hero of every player who has finished joining, then step
    // the match.
    this.now = seconds
    this.fighters.frame(seconds)
    for (const fighter of this.fighters.members()) {
      this.follow(fighter)
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
    const fighter = this.fighters.get(player)
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
    const fighter = this.fighters.get(player)
    if (fighter?.entered) {
      fighter.entered = false
      fighter.player.freeze(false)
      toast(player, 'You are watching. Type /ready to fight again.')
    }
  }

  /**
   * addBot adds a practice bot that enters at once, and returns a message
   * saying whether it joined.
   */
  addBot(): string {
    // Alternate the bots between the two teams.
    const name = `Arena Bot ${this.bots.length + 1}`
    const team = 2 + (this.bots.length % 2)
    const bot = addBot({ name, team, hero: botHero })
    if (bot === undefined) {
      return 'The bot could not join.'
    }

    // When the bot connects, the cast calls join, which enters it at once.
    this.bots.push(bot)
    return `${name} joins the arena.`
  }

  /** removeBots removes every practice bot. */
  removeBots(): string {
    for (const bot of this.bots) {
      bot.removeBot()
    }
    const count = this.bots.length
    this.bots.length = 0
    return count === 0 ? 'There are no bots.' : 'The bots leave.'
  }

  /** entrants lists the fighters who typed /ready. */
  entrants(): Fighter[] {
    return this.fighters.members().filter((fighter) => fighter.entered)
  }

  /** fighterAt returns the fighter whose hero is the entity pawn. */
  private fighterAt(pawn: number): Fighter | undefined {
    if (pawn === 0) {
      return undefined
    }
    return this.fighters.members().find((fighter) => fighter.pawn === pawn)
  }

  /**
   * join makes the fighter for a new connection. A practice bot enters at
   * once, while a person is shown the rules and enters by typing /ready.
   */
  private join({ player, name, bot }: Connection): Fighter {
    if (!bot) {
      player.announce(
        'ARENA',
        `Every hero is an opponent. First to ${killTarget} defeats wins. Type /ready to fight.`,
      )
    }
    return {
      player,
      name,
      entered: bot,
      pawn: 0,
      alive: false,
      kills: 0,
      deaths: 0,
      streak: 0,
      protectedUntil: 0,
    }
  }

  /**
   * follow records whether fighter's hero is alive. When the hero appears or
   * comes back, follow turns on friendly fire for it and blocks hits on it
   * for protectionSeconds. The cast respawns a fallen hero itself.
   */
  private follow(fighter: Fighter): void {
    // A player without a hero has nothing to follow yet.
    const pawn = this.fighters.pawn(fighter.player)
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
    fighter.pawn = pawn.entity
    fighter.alive = alive
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
    for (const fighter of this.fighters.members()) {
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
    for (const fighter of this.fighters.members()) {
      fighter.player.announce(title, text)
    }
  }

  /** tell shows every player a short notice. */
  private tell(text: string): void {
    for (const fighter of this.fighters.members()) {
      toast(fighter.player, text)
    }
  }
}
