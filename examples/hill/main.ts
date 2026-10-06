// King of the Hill: a player types /hill to raise the hill where their hero
// stands. Each second, a team alone on the hill scores a point; heroes from
// both teams contest it. The first team to sixty points wins the round, and
// the next round starts on the same hill.
import {
  command,
  onFrame,
  onStart,
  onWorld,
  serverCommand,
  toast,
} from 'modlock'

import { Hill } from './hill'
import { drawBars } from './hud'

const hill = new Hill()

// A server that hibernates when empty runs no frames, so the hill would stop.
onStart(() => {
  serverCommand('sv_hibernate_when_empty 0')
})

// A new world has removed the markers.
onWorld(() => hill.forget())

onFrame((frame) => {
  hill.frame(frame.timeSeconds)
  drawBars(hill)
})

command('hill', (player) => {
  const pawn = player.pawn()
  if (pawn === undefined || pawn.health <= 0) {
    toast(player, 'You need a living hero to raise the hill.')
    return
  }
  if (!hill.raise(pawn.position)) {
    toast(player, 'The hill could not be raised here.')
  }
})
