// Cooldown checks setAbility's cooldown, the cast events and the kill event.
// When the server starts the mod with the argument probe, the mod adds two
// bots on the same hero, gives one bot's first three abilities a cooldown of
// its own, and has both press their ability keys in turn. Each cooldown the
// game starts for the changed bot should last the given seconds, cast after
// cast, while the other bot keeps the game's own, and each cast should arrive
// as an event naming the bot and the ability. One bot then kills the other,
// which should arrive as a kill naming both. The mod quits the server with
// PASS or FAIL.
import {
  addBot,
  Buttons,
  killEntity,
  log,
  onAbilityCast,
  onFrame,
  onKill,
  onModifierEvent,
  onStart,
  serverCommand,
  type Player,
} from 'modlock'

/**
 * hero is the bots' hero. Its first ability holds charges, which the check
 * gives both bots, and its third has a plain cooldown, so the check covers
 * both kinds.
 */
const hero = 'hero_wraith'

/** length is the cooldown, in seconds, the check gives the changed bot. */
const length = 3

/** charges is how many charges of its first ability the check gives each bot. */
const charges = 20

/** tolerance is how far a measured cooldown may be from length. */
const tolerance = 0.1

/** duration is how many seconds the bots press their keys. */
const duration = 24

/** timeout is how many seconds the whole check may take before it fails. */
const timeout = 60

/** keys are the ability keys the bots press, one each half second. */
const keys = [Buttons.ability1, Buttons.ability2, Buttons.ability3]

/** watched are the modifier events the check counts. */
const watched = [
  'ABILITY_CAST_STARTED',
  'MODIFIER_EVENT_ABILITY_EXECUTED',
  'PLAYER_RESPAWNED',
  'PLAYER_KILLED',
  'KILLED',
  'MELEE_ATTACK',
  'HERO_LEVEL_CHANGED',
  'UNIT_HEALED',
  'PLAYER_RELOAD_STARTED',
  'BULLET_HIT',
]

/** Timer is the latest start the check saw for one of an ability's timers. */
interface Timer {
  start: number
  lengths: number[]
}

/** Bot is one seated bot with what the check measured for it. */
interface Bot {
  readonly label: string
  readonly player: Player
  readonly changed: boolean
  readonly cooldowns: Map<string, Timer>
  casts: number
}

const bots: Bot[] = []
const fired = new Map<string, number>()
let started: number | undefined
let done = false
let probing = false
let unlockedAt: number | undefined
let armedAt: number | undefined
let pressed = -1
let revived = -1
let unnamed = 0
let killedAt: number | undefined
let killSeen = false

onStart((args) => {
  probing = args.includes('probe')
})

onAbilityCast((cast) => {
  // Count the cast for its bot, and note one that names no ability.
  const bot = bots.find((item) => item.player.slot === cast.player.slot)
  if (bot === undefined) {
    return
  }
  bot.casts++
  if (cast.name === '' || cast.slot < 0) {
    unnamed++
  }
  log(
    `cast by ${bot.label}: ${cast.name} slot ${cast.slot} ability ${cast.ability} target ${cast.target}`,
  )
})

onKill((kill) => {
  // The control bot's kill of the changed bot names both players.
  const victim = bots.find((bot) => bot.player === kill.victimPlayer)
  const attacker = bots.find((bot) => bot.player === kill.attackerPlayer)
  log(
    `kill: ${attacker?.label ?? kill.attackerPlayer.slot} killed ${victim?.label ?? kill.victimPlayer.slot} with ${kill.ability}`,
  )
  if (victim?.changed === true && attacker?.changed === false) {
    killSeen = true
  }
})

for (const name of watched) {
  onModifierEvent(name, (event) => {
    // Log the first of each event with its entities, and count the rest.
    const count = fired.get(name) ?? 0
    fired.set(name, count + 1)
    if (count === 0) {
      log(
        `first ${event.name}: caster ${event.caster} player ${event.player.slot} target ${event.target} ability ${event.ability}`,
      )
    }
  })
}

/** seat adds the two bots once the server is up. */
function seat(): boolean {
  for (const changed of [false, true]) {
    const label = changed ? 'changed' : 'control'
    const player = addBot({ name: `Cooldown ${label}`, team: 2, hero })
    if (player === undefined) {
      finish(`the ${label} bot could not join`)
      return false
    }
    bots.push({
      label,
      player,
      changed,
      cooldowns: new Map(),
      casts: 0,
    })
  }
  return true
}

/** measure records a timer's length the first time the check sees its start. */
function measure(
  timers: Map<string, Timer>,
  name: string,
  start: number,
  end: number,
): void {
  const timer = timers.get(name) ?? { start: 0, lengths: [] }
  timers.set(name, timer)
  if (end > start && start !== timer.start) {
    timer.start = start
    timer.lengths.push(end - start)
  }
}

onFrame((frame) => {
  // Do nothing unless the probe runs.
  if (done || !probing) {
    return
  }

  // Seat the bots on the first frame, once the server is up.
  if (bots.length === 0 && !seat()) {
    return
  }
  started ??= frame.timeSeconds
  const elapsed = frame.timeSeconds - started
  if (elapsed > timeout) {
    finish('timed out')
    return
  }

  // Wait until every bot has a hero with abilities.
  const abilities = bots.map((bot) => bot.player.abilities())
  if (abilities.some((list) => list.length === 0)) {
    return
  }

  // Wait until every bot's hero is alive, reviving the dead once a second.
  const dead = bots.filter((bot) => (bot.player.pawn()?.health ?? 0) <= 0)
  if (dead.length > 0 && killedAt === undefined) {
    if (Math.floor(elapsed) !== revived) {
      revived = Math.floor(elapsed)
      for (const bot of dead) {
        bot.player.respawn()
      }
    }
    return
  }

  // Unlock each bot's first three abilities, then two seconds later give both
  // bots charges of the first and the changed bot's abilities a cooldown.
  if (unlockedAt === undefined) {
    unlockedAt = frame.timeSeconds
    for (const [index, bot] of bots.entries()) {
      for (const ability of abilities[index].filter((item) => item.slot < 3)) {
        bot.player.setAbility({ ability: ability.name, upgrades: 1 })
      }
    }
    return
  }
  if (frame.timeSeconds - unlockedAt < 2) {
    return
  }
  if (armedAt === undefined) {
    armedAt = frame.timeSeconds
    for (const [index, bot] of bots.entries()) {
      bot.player.setAbility({
        ability: abilities[index][0].name,
        maxCharges: charges,
        charges,
      })
    }
    const changed = bots.findIndex((bot) => bot.changed)
    for (const ability of abilities[changed].filter((item) => item.slot < 3)) {
      const set = bots[changed].player.setAbility({
        ability: ability.name,
        cooldown: length,
      })
      log(`${ability.name} cooldown ${length}: ${set ? 'set' : 'refused'}`)
    }
    return
  }

  // Record each cooldown the game started.
  for (const [index, bot] of bots.entries()) {
    for (const ability of abilities[index].filter((item) => item.slot < 3)) {
      measure(
        bot.cooldowns,
        ability.name,
        ability.cooldownStart,
        ability.cooldownEnd,
      )
    }
  }

  // Press one ability key each half second, in turn, then give the verdict.
  const armed = frame.timeSeconds - armedAt
  const turn = Math.floor(armed * 2)
  if (armed < duration) {
    if (turn !== pressed) {
      pressed = turn
      for (const bot of bots) {
        bot.player.press(keys[turn % keys.length])
      }
    }
    return
  }

  // The control bot then kills the changed one, and a second later the check
  // gives its verdict.
  if (killedAt === undefined) {
    killedAt = frame.timeSeconds
    const [control, changed] = bots.map((bot) => bot.player.pawn()?.entity ?? 0)
    killEntity({ victim: changed, attacker: control })
    return
  }
  if (frame.timeSeconds - killedAt < 1) {
    return
  }
  finish(verdict())
})

/** verdict returns what the check found wrong, or nothing. */
function verdict(): string | undefined {
  // Log every length the check measured.
  for (const bot of bots) {
    for (const [name, timer] of bot.cooldowns) {
      if (timer.lengths.length > 0) {
        log(
          `${bot.label} ${name} cooldown: ${timer.lengths.map((value) => value.toFixed(2)).join(' ')}`,
        )
      }
    }
    log(`${bot.label} casts: ${bot.casts}`)
  }
  for (const name of watched) {
    log(`${name} fired ${fired.get(name) ?? 0} times`)
  }

  // Every cast names its ability, and both bots cast.
  if (bots.some((bot) => bot.casts === 0)) {
    return 'a bot cast nothing'
  }
  if (!killSeen) {
    return 'the kill did not arrive'
  }
  if (unnamed > 0) {
    return `${unnamed} casts named no ability`
  }

  // Gather each bot's measured lengths.
  const lengths = (bot: Bot) =>
    [...bot.cooldowns.values()].flatMap((timer) => timer.lengths)
  const changed = bots.find((bot) => bot.changed)!
  const control = bots.find((bot) => !bot.changed)!

  // The changed bot's cooldowns last the given seconds.
  const off = lengths(changed).filter(
    (value) => Math.abs(value - length) > tolerance,
  )
  if (off.length > 0) {
    return `the changed bot had cooldowns of ${off.map((value) => value.toFixed(2)).join(' ')}`
  }

  // The ability with charges and the plain one each start more than once,
  // and none of the control bot's matches.
  const owned = changed.player.abilities()
  const repeated = [0, 2].every((slot) => {
    const timer = changed.cooldowns.get(owned[slot]?.name ?? '')
    return (timer?.lengths.length ?? 0) >= 2
  })
  if (!repeated) {
    return 'a cooldown of the changed bot did not start twice'
  }
  if (lengths(control).some((value) => Math.abs(value - length) <= tolerance)) {
    return 'the control bot had the same cooldowns'
  }
  return undefined
}

/** finish logs the verdict and quits the server. */
function finish(problem?: string): void {
  done = true
  log(
    problem === undefined
      ? 'Cooldown probe PASS'
      : `Cooldown probe FAIL: ${problem}`,
  )
  serverCommand('quit')
}
