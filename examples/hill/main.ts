// King of the Hill: a player types /hill to raise the hill where their hero
// stands. Each second, a team scores a point when only its heroes stand on
// the hill, and nobody scores while heroes from both teams stand there. The
// first team to sixty points wins the round, and the next round starts on the
// same hill.
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

// An empty server hibernates and skips frames, which would stop the hill.
onStart(() => {
  serverCommand('sv_hibernate_when_empty 0')
})

// Loading a new map removes the markers, so the hill forgets them.
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
