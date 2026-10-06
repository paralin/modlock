// hello-ts shows the parts of a Modlock mod in TypeScript. Players type
// /hello to be greeted in their chosen words, /sign to raise floating text
// over their hero, /where to see where they stand, and /menu to open a panel
// with their hero's health and buttons that act on the game.
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

// Log the arguments a server passes after -- on its command line.
onStart((args) => {
  log('hello-ts started with', args.length, 'arguments:', args.join(' '))
})

// A new world has removed every object, so forget the old signs.
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

// Tell a player when they change a setting on their profile.
onSettingChanged((player, key, value) => {
  if (key === 'greeting') {
    toast(player, `You will be greeted with "${greetings[value] ?? value}".`)
  }
})

// Bob the signs and keep open menus current.
onFrame((frame) => {
  bobSigns(frame.timeSeconds)
  drawMenus()
})
