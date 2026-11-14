// Charge limit checks setAbility's maxCharges. When the server starts the mod
// with the argument probe, the mod adds two bots on a hero with a charged
// ability, raises one bot's maximum charges, empties the ability on both, and
// keeps each recharge short. The raised bot should refill to its new maximum
// and the other to the game's own. The mod quits the server with PASS or FAIL.
import {
  addBot,
  log,
  onFrame,
  onStart,
  serverCommand,
  type Player,
} from 'modlock'

/** hero is the bots' hero, whose Card Toss holds two charges. */
const hero = 'hero_wraith'

/**
 * target is the ability the check empties, and the game's scripts give it
 * baseCharges charges.
 */
const target = 'citadel_ability_card_toss'
const baseCharges = 2

/** extra is how many charges the check adds to the raised bot's maximum. */
const extra = 2

/** timeout is how many seconds the whole check may take before it fails. */
const timeout = 40

/** shortest is how many seconds the check lets each recharge take. */
const shortest = 0.5

/** Bot is one seated bot and the ability the check watches. */
interface Bot {
  readonly label: string
  readonly player: Player
  readonly raised: boolean
  ability?: string
}

const bots: Bot[] = []
let started: number | undefined
let native = 0
let done = false
let probing = false
let unlocked = false
let logged = -1
let revived = -1
let armedAt = 0
let unlockedAt = 0

onStart((args) => {
  probing = args.includes('probe')
})

/** seat adds the two bots once the server is up. */
function seat(): boolean {
  for (const [index, raised] of [false, true].entries()) {
    const label = raised ? 'raised' : 'control'
    const player = addBot({ name: `Charge ${label}`, team: 2 + index, hero })
    if (player === undefined) {
      finish(`the ${label} bot could not join`)
      return false
    }
    bots.push({ label, player, raised })
  }
  return true
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
  if (dead.length > 0) {
    if (Math.floor(elapsed) !== revived) {
      revived = Math.floor(elapsed)
      for (const bot of dead) {
        bot.player.respawn()
      }
      log(`reviving ${dead.map((bot) => bot.label).join(' ')}`)
    }
    return
  }

  // Unlock each bot's first three abilities, because a locked ability has zero
  // charges. Two seconds later, empty the target ability on both bots and raise
  // the maximum of one.
  if (native === 0) {
    if (!unlocked) {
      unlocked = true
      unlockedAt = frame.timeSeconds
      for (const bot of bots) {
        for (const ability of bot.player.abilities()) {
          if (ability.slot < 3) {
            bot.player.setAbility({ ability: ability.name, upgrades: 1 })
          }
        }
      }
      return
    }
    if (frame.timeSeconds - unlockedAt < 2) {
      return
    }
    const charged = abilities[0].find((item) => item.name === target)
    if (charged === undefined) {
      finish(`the hero has no ${target}`)
      return
    }
    native = baseCharges
    armedAt = frame.timeSeconds
    for (const bot of bots) {
      bot.ability = charged.name
      bot.player.setAbility({
        ability: charged.name,
        ...(bot.raised ? { maxCharges: native + extra } : {}),
        charges: 0,
        rechargeEnd: frame.timeSeconds + shortest,
      })
    }
    log(`${charged.name} holds ${native} charges; raising one bot by ${extra}`)
    return
  }

  // Keep each recharge short until the check ends.
  const held: number[] = []
  for (const [index, bot] of bots.entries()) {
    const ability = abilities[index].find((item) => item.name === bot.ability)
    if (ability === undefined) {
      finish(`the ${bot.label} bot lost its ability`)
      return
    }
    held.push(ability.charges)
    // The game starts a recharge only after a use, so the check starts one.
    if (
      ability.rechargeEnd === 0 ||
      ability.rechargeEnd > frame.timeSeconds + shortest
    ) {
      bot.player.setAbility({
        ability: ability.name,
        rechargeEnd: frame.timeSeconds + shortest,
      })
    }
  }
  if (Math.floor(elapsed) !== logged) {
    logged = Math.floor(elapsed)
    log(`t=${elapsed.toFixed(1)} control=${held[0]} raised=${held[1]}`)
  }
  if (frame.timeSeconds - armedAt > 15) {
    const ok = held[0] === native && held[1] === native + extra
    finish(
      ok ? undefined : `control held ${held[0]}, raised held ${held[1]}`,
      `native ${native}`,
    )
  }
})

/** finish logs the verdict and quits the server. */
function finish(problem?: string, note = ''): void {
  done = true
  log(
    problem === undefined
      ? `Charge limit probe PASS ${note}`
      : `Charge limit probe FAIL: ${problem}`,
  )
  serverCommand('quit')
}
