// Target Drill: a player types /drill and three target heroes appear in
// front of them. Each hit scores a point, a headshot two, and the target
// hops to a new spot. When the clock runs out, the player sees their score
// and their best is kept. /stop ends a drill early.
import {
  command,
  onDamage,
  onFrame,
  onStart,
  onWorld,
  serverCommand,
  toast,
} from 'modlock'

import { Drill, serverConfig } from './drill'

/** drills holds the running drill of each shooter, by player slot. */
const drills = new Map<number, Drill>()

/** now is the game clock at the latest frame. */
let now = 0

onStart(() => {
  for (const line of serverConfig) {
    serverCommand(line)
  }
})

// A new world has removed every target.
onWorld(() => drills.clear())

// Targets never take damage; a drill scores the shots that reach them.
onDamage((hit) => {
  for (const drill of drills.values()) {
    if (drill.hit(hit)) {
      return { block: true }
    }
  }
})

onFrame((frame) => {
  now = frame.timeSeconds
  for (const drill of drills.values()) {
    if (now >= drill.endsAt || drill.shooter.pawn() === undefined) {
      finish(drill)
      continue
    }
    drill.frame()
    const left = Math.ceil(drill.endsAt - now)
    drill.shooter.centerText(`${drill.score} points · ${left}s`)
  }
})

command('drill', (player) => {
  // A drill needs a living hero and starts where it stands and aims.
  const pawn = player.pawn()
  if (drills.has(player.slot)) {
    toast(player, 'Your drill is already running. Type /stop to end it.')
    return
  }
  if (pawn === undefined || pawn.health <= 0) {
    toast(player, 'You need a living hero to start a drill.')
    return
  }

  // The setting chooses the length; the targets join the other team.
  const seconds = Number(player.setting('length') ?? '30')
  const drill = new Drill(
    player,
    now + seconds,
    pawn.position,
    pawn.eyeAngles.yaw,
  )
  if (!drill.start(pawn.team)) {
    toast(player, 'The targets could not join.')
    return
  }
  drills.set(player.slot, drill)
  toast(player, 'Hit the targets. Headshots score double.')
})

command('stop', (player) => {
  const drill = drills.get(player.slot)
  if (drill) {
    finish(drill)
  }
})

/** finish ends a drill, shows the shooter the result and keeps their best. */
function finish(drill: Drill): void {
  // Remove the targets and the running score.
  const { shooter, score, headshots } = drill
  drill.stop()
  drills.delete(shooter.slot)
  shooter.centerText('')

  // Show the result and keep it.
  shooter.announce('Drill over', `${score} points, ${headshots} headshots`)
  shooter.addMetric('drill.score', score)
  shooter.addMetric('drill.headshots', headshots)
}
