// Arena is a free-for-all on any map. Players type /ready to fight and
// /leave to watch, and any player can type /bot to add a practice bot or
// /nobots to remove them all. When two fighters are ready a countdown starts,
// the first to ten defeats wins, and the arena then waits for the next match.
// When the server starts the mod with the argument probe, the mod checks
// itself with two bots and quits with PASS or FAIL.
import {
  command,
  onDamage,
  onDamaged,
  onFrame,
  onStart,
  onWorld,
  toast,
} from 'modlock'

import { drawScoreboards } from './hud'
import { Match } from './match'
import { Probe } from './probe'

const match = new Match()
let probe: Probe | undefined

onStart((args) => {
  match.start()
  if (args.includes('probe')) {
    probe = new Probe()
  }
})

onWorld(() => match.world())
onDamage((hit) => match.damage(hit))
onDamaged((hit) => match.damaged(hit))

onFrame((frame) => {
  match.frame(frame.timeSeconds)
  drawScoreboards(match)
  probe?.tick(match, frame.timeSeconds)
})

command('ready', (player) => match.ready(player))
command('leave', (player) => match.leave(player))
command('bot', (player) => toast(player, match.addBot()))
command('nobots', (player) => toast(player, match.removeBots()))
