// Street Brawl checks the calls a Street Brawl style mode builds on. Started
// with the argument probe, the mod seats a bot and puts its hero on each
// lane's zipline in turn, from wherever the last ride left it, and checks that
// the nearest zipline node is on the asked lane, and that green, which the
// three-lane map lacks, is refused. It then times the ride with
// and without Street Brawl's zipline boost, and imbues an ability with an
// item. The mod quits the server with PASS or FAIL.
import {
  addBot,
  entityPosition,
  findEntities,
  log,
  onFrame,
  onStart,
  readField,
  serverCommand,
  type Lane,
  type Player,
  type Vector,
} from 'modlock'

/** lanes lists the map's lanes to ride, each far from the one before. */
const lanes: readonly Lane[] = ['yellow', 'purple', 'blue']

/** missing is a lane the map has no zipline on. */
const missing: Lane = 'green'

/** laneNumbers is the number the game gives each lane's zipline nodes. */
const laneNumbers: Record<Lane, number> = {
  none: 0,
  yellow: 1,
  green: 3,
  blue: 4,
  purple: 6,
}

/** boost is Street Brawl's fast zipline modifier. */
const boost = 'modifier_street_brawl_round_start_zipline_boost'

/** imbuing is an item that imbues an ability, and plain one that does not. */
const imbuing = 'upgrade_arcane_extension'
const plain = 'upgrade_fleetfoot_boots'

/** timeout bounds the whole check in seconds. */
const timeout = 90

let probing = false
let bot: Player | undefined
let started: number | undefined
let step = 0
let stepAt = 0
let mark: Vector | undefined
let plainSpeed = 0
const problems: string[] = []
let done = false

onStart((args) => {
  probing = args.includes('probe')
})

onFrame((frame) => {
  // Do nothing unless the probe runs.
  if (done || !probing) {
    return
  }
  const now = frame.timeSeconds
  started ??= now
  if (now - started > timeout) {
    finish(`timed out at step ${step}`)
    return
  }

  // Seat the bot, then wait until its hero lives.
  bot ??= addBot({ name: 'Street Brawl', team: 2, hero: 'hero_wraith' })
  if (bot === undefined) {
    finish('the bot could not join')
    return
  }
  const pawn = bot.pawn()
  if (pawn === undefined || pawn.health <= 0 || bot.abilities().length === 0) {
    stepAt = now
    return
  }

  // Ride each lane, two seconds apart, and check the lane under the hero.
  if (step < 2 * lanes.length) {
    const lane = lanes[Math.floor(step / 2)]
    if (step % 2 === 0) {
      if (now - stepAt < 1) {
        return
      }
      if (step === 0) {
        logLanes()
        if (bot.zipline(missing)) {
          problems.push(`zipline ${missing} rode a lane the map lacks`)
        }
      }
      if (!bot.zipline(lane)) {
        problems.push(`zipline ${lane} failed`)
      }
    } else {
      if (now - stepAt < 2) {
        return
      }
      const [node, distance] = nearestNode(pawn.position)
      const number = laneOf(node)
      log(
        `asked ${lane}: nearest node lane ${number} at ${distance.toFixed(0)}`,
      )
      if (number !== laneNumbers[lane] || distance > 600) {
        problems.push(
          `asked ${lane}, rode lane ${number} at ${distance.toFixed(0)}`,
        )
      }
    }
    step++
    stepAt = now
    return
  }

  // Time a second of the ride, then a second with the boost.
  const ride = step - 2 * lanes.length
  if (ride < 4) {
    if (now - stepAt < 1) {
      return
    }
    if (ride === 0 || ride === 2) {
      if (ride === 2 && !bot.giveModifier(boost, 8)) {
        problems.push('the boost was not given')
      }
      if (!bot.zipline('yellow')) {
        problems.push('the timed ride failed')
      }
      mark = undefined
    } else if (mark === undefined) {
      mark = pawn.position
      stepAt = now
      return
    } else {
      const speed = distanceOf(mark, pawn.position) / (now - stepAt)
      log(
        `${ride === 1 ? 'plain' : 'boosted'} ride ${speed.toFixed(0)} units/s`,
      )
      if (ride === 1) {
        plainSpeed = speed
      } else if (speed < 1.3 * plainSpeed) {
        problems.push(
          `boosted ride ${speed.toFixed(0)} is not faster than ${plainSpeed.toFixed(0)}`,
        )
      }
    }
    step++
    stepAt = now
    return
  }

  // Imbue the first ability that accepts the item, and refuse a plain one.
  if (bot.giveItem(plain, 0)) {
    problems.push(`${plain} imbued an ability`)
  }
  const slot = [0, 1, 2, 3].find((index) => bot?.giveItem(imbuing, index))
  const abilities = bot.abilities()
  const item = abilities.find((ability) => ability.name === imbuing)
  const target = abilities.find((ability) => ability.slot === slot)
  log(
    `${imbuing} in slot ${slot} imbues ${item?.imbues.join(' ') ?? 'nothing'}`,
  )
  if (
    slot === undefined ||
    target === undefined ||
    !item?.imbues.includes(target.name)
  ) {
    problems.push(`${imbuing} did not imbue ${target?.name ?? 'any ability'}`)
  }
  finish(problems.length > 0 ? problems.join('; ') : undefined)
})

/** logLanes logs how many zipline nodes each lane number and team has. */
function logLanes(): void {
  const counts = new Map<string, number>()
  for (const node of findEntities('zip_line_node')) {
    const team = readField(node, 'CBaseEntity', 'm_iTeamNum', 'uint8')
    const key = `lane ${laneOf(node)} team ${typeof team === 'number' ? team : -1}`
    counts.set(key, (counts.get(key) ?? 0) + 1)
  }
  log([...counts].map(([key, count]) => `${key}: ${count}`).join(', '))
}

/** laneOf reads a zipline node's lane number, or -1 when it cannot. */
function laneOf(node: number): number {
  const lane = readField(node, 'CCitadelZipLineNode', 'm_iPrimaryLane', 'int16')
  return typeof lane === 'number' ? lane : -1
}

/** nearestNode returns the zipline node nearest position and its distance. */
function nearestNode(position: Vector): [number, number] {
  let nearest: [number, number] = [0, Infinity]
  for (const node of findEntities('zip_line_node')) {
    const at = entityPosition(node)?.position
    const distance = at === undefined ? Infinity : distanceOf(at, position)
    if (distance < nearest[1]) {
      nearest = [node, distance]
    }
  }
  return nearest
}

/** distanceOf returns how far apart two points are. */
function distanceOf(a: Vector, b: Vector): number {
  return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z)
}

/** finish logs the verdict and quits the server. */
function finish(problem?: string): void {
  done = true
  log(
    problem === undefined
      ? 'Street Brawl probe PASS'
      : `Street Brawl probe FAIL: ${problem}`,
  )
  serverCommand('quit')
}
