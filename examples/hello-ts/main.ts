// hello-ts shows the parts of a Modlock mod in TypeScript. A player types
// /hello to be greeted in the words their greeting setting chooses, /sign to
// raise floating text over their hero, /where to see where their hero stands,
// and /menu to open a panel with their hero's health and buttons that call
// back into the mod.
import {
  command,
  log,
  onFrame,
  onSettingChanged,
  onStart,
  onWorld,
  toast,
} from 'modlock'

import {
  bobSigns,
  forgetSigns,
  greet,
  greetings,
  nameOf,
  placeSign,
} from './hello'
import { closeMenu, drawMenus, openMenu } from './menu'

// When the server starts the mod, log the arguments its operator wrote after
// `--` on the server's command line.
onStart((args) => {
  log('hello-ts started with', args.length, 'arguments:', args.join(' '))
})

// Loading a map removes every sign from the game. The mod forgets them too.
onWorld((map) => {
  forgetSigns()
  log('world loaded:', map)
})

command('hello', (player, args) => {
  greet(player, args === '' ? nameOf(player) : args)
})

command('sign', (player, args) => {
  if (!placeSign(player, args === '' ? nameOf(player) : args)) {
    toast(player, 'You need a living hero to hold a sign.')
  }
})

command('where', (player) => {
  const pawn = player.pawn()
  if (pawn === undefined) {
    toast(player, 'You have no hero yet.')
    return
  }
  const { x, y, z } = pawn.position
  toast(
    player,
    `You stand at ${x.toFixed(0)}, ${y.toFixed(0)}, ${z.toFixed(0)}.`,
  )
})

command('menu', (player) => {
  openMenu(player)
})

command('close', (player) => {
  closeMenu(player)
})

// When a player changes their greeting setting on their profile, tell them
// the words they will now hear.
onSettingChanged((player, key, value) => {
  if (key === 'greeting') {
    toast(player, `You will be greeted with "${greetings[value] ?? value}".`)
  }
})

// On every frame, bob each sign and redraw each open menu with its player's
// current health and souls.
onFrame((frame) => {
  bobSigns(frame.timeSeconds)
  drawMenus()
})
