// controllers shows a player the game controllers their own Modlock client
// streams to the server. A player types /controllers to start watching their
// devices and again to stop; while they watch, a panel lists each device with
// its axes, held buttons, hats and gamepad sticks, and the state of the
// stream that brings them. Their client opens the devices only while the
// watch holds.
import {
  command,
  hide,
  onFrame,
  type Player,
  type PlayerControllers,
  players,
  toast,
} from 'modlock'

import { drawPanel } from './panel'

/** watching holds the slot of each player who watches their controllers. */
const watching = new Set<number>()

command('controllers', (player) => {
  if (watching.delete(player.slot)) {
    stop(player)
    return
  }
  if (!player.watchControllers(true)) {
    toast(player, 'This server cannot read controllers.')
    return
  }
  watching.add(player.slot)
})

/** stop ends player's watch, closing their devices, and hides the panel. */
function stop(player: Player): void {
  player.watchControllers(false)
  hide(player)
}

// On every frame, draw each watching player's controllers, and forget the
// players who left.
onFrame((frame) => {
  // Forget the watchers who left; their watch ended with them.
  const present = new Set(players().map((c) => c.player.slot))
  for (const slot of watching) {
    if (!present.has(slot)) {
      watching.delete(slot)
    }
  }

  // Draw each watcher's devices.
  const bySlot = new Map<number, PlayerControllers>()
  for (const entry of frame.controllers) {
    bySlot.set(entry.player.slot, entry)
  }
  for (const connection of players()) {
    if (watching.has(connection.player.slot)) {
      drawPanel(connection.player, bySlot.get(connection.player.slot))
    }
  }
})
