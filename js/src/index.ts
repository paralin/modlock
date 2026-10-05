// The modlock module writes Modlock mods in TypeScript and JavaScript. A mod
// registers its handlers when its bundle loads; modlock-host runs the bundle
// on QuickJS in a sandbox inside the game server and calls the handlers on the
// server's frame thread, one at a time.
//
//   import { command } from 'modlock'
//
//   command('hello', (player) => {
//     player.chat('Hello from TypeScript!')
//   })
//
// Build it with modlock build.
//
// The calls into the game and their types are generated from the schema in
// host.gen.ts. A call that fails logs why in the server log and returns
// false or undefined, so a mod can retry on a later frame instead of
// stopping.

import {
  type DamageEvent,
  type DamagedEvent,
  type DamageResult,
  type FrameEvent,
  type ImpactEvent,
  type LandedEvent,
  type LaunchEvent,
  log as logLine,
  type ModHandlers,
  Player,
  serveMod,
} from './host.gen.js'
import { host } from './host.js'
import { press, tick } from './ui.js'

export { Buttons, Layers } from './bits.js'
export * from './host.gen.js'
export { type CatalogEntry, Dropper, dropperButtons } from './dropper.js'
export { decodeSpot, encodeSpot, LoadedSpot, loadSpot, type Placed, type Spot, type SpotObject, triggers } from './spot.js'
export {
  type ButtonProps,
  type Child,
  type Component,
  type Edges,
  Fragment,
  hide,
  type ImageProps,
  JSX,
  jsx,
  jsxs,
  type LabelProps,
  type Length,
  type PanelProps,
  show,
  type Style,
  toast,
  type UiElement,
} from './ui.js'

/** log writes one line to the server log under the mod's name. */
export function log(...parts: unknown[]): void {
  logLine(parts.map(String).join(' '))
}

/**
 * Service answers the host's calls to one service the mod serves: the
 * method's name and payload in, the answer out. A thrown error becomes the
 * call's error.
 */
export type Service = (method: string, payload: Uint8Array) => Uint8Array

/** serve answers the host's calls to service with handler, replacing its earlier handler. */
export function serve(service: string, handler: Service): void {
  services.set(service, handler)
}

/**
 * command calls handler when a player types /name in chat or their client
 * sends the console command name to the server. The handler receives the
 * text after the name, trimmed of surrounding spaces. Registering a name
 * again replaces its handler.
 */
export function command(name: string, handler: (player: Player, args: string) => void): void {
  commands.set(name, handler)
}

/**
 * onStart calls handler once when the server starts the mod, with the
 * arguments that follow -- on the modlock-host command line.
 */
export function onStart(handler: (args: readonly string[]) => void): void {
  starts.push(handler)
}

/** onFrame calls handler once per server frame. */
export function onFrame(handler: (frame: FrameEvent) => void): void {
  frames.push(handler)
}

/**
 * onWorld calls handler with the map's name each time a world loads, and at
 * start when one already has. Objects and bots from an earlier world are
 * gone by then.
 */
export function onWorld(handler: (map: string) => void): void {
  worlds.push(handler)
}

/**
 * onInput calls handler with the watched buttons a player pressed and
 * released, as Buttons bits, before the next frame. A button is held from its
 * press until its release.
 */
export function onInput(handler: (player: Player, pressed: bigint, released: bigint) => void): void {
  inputs.push(handler)
}

/**
 * onRestored calls handler when a player's Player.restoreHero ends: error is
 * undefined once the target held for a second, or says why it did not.
 */
export function onRestored(handler: (player: Player, error: string | undefined) => void): void {
  restoreds.push(handler)
}

/**
 * onNpcsRestored calls handler when restoreNpcs ends: error is undefined once
 * the map holds every target, or says why it does not.
 */
export function onNpcsRestored(handler: (error: string | undefined) => void): void {
  npcsRestoreds.push(handler)
}

/**
 * onDamage calls handler before each hit lands, so it can block the hit or
 * change its damage. Later handlers see the earlier ones' amount.
 */
export function onDamage(handler: (hit: DamageEvent) => DamageResult | void): void {
  damages.push(handler)
}

/** onDamaged calls handler on the frame after each hit lands. */
export function onDamaged(handler: (hit: DamagedEvent) => void): void {
  damageds.push(handler)
}

/**
 * onLaunch calls handler with each watched projectile's first frame, before
 * the next frame. watchProjectiles names the projectiles.
 */
export function onLaunch(handler: (projectile: LaunchEvent) => void): void {
  launches.push(handler)
}

/**
 * onImpact calls handler when a watched projectile strikes something, inside
 * the game's impact: its calls apply before the game continues.
 */
export function onImpact(handler: (impact: ImpactEvent) => void): void {
  impacts.push(handler)
}

/**
 * onLanded calls handler when a hero lands under the manifest's QuakeWorld
 * movement, before the next frame.
 */
export function onLanded(handler: (landing: LandedEvent) => void): void {
  landeds.push(handler)
}

/**
 * onSettingChanged calls handler when a player's setting changes outside the
 * mod, such as on the player's profile. The mod's own player.setSetting calls
 * do not reach it.
 */
export function onSettingChanged(
  handler: (player: Player, key: string, value: string) => void,
): void {
  settingChanges.push(handler)
}

/**
 * settingOn returns the player's value of a switch setting the manifest
 * declares, or undefined when the host refuses the read.
 */
export function settingOn(player: Player, key: string): boolean | undefined {
  const value = player.setting(key)
  return value === undefined ? undefined : value === 'true'
}

/**
 * settingNumber returns the player's value of a number setting the manifest
 * declares, or undefined when the host refuses the read.
 */
export function settingNumber(player: Player, key: string): number | undefined {
  const value = player.setting(key)
  return value === undefined ? undefined : Number(value)
}

/** commands maps each registered command name to its handler. */
const commands = new Map<string, (player: Player, args: string) => void>()

/** services maps each service the mod serves to its handler. */
const services = new Map<string, Service>()

// Each list runs in registration order.
const starts: ((args: readonly string[]) => void)[] = []
const frames: ((frame: FrameEvent) => void)[] = []
const worlds: ((map: string) => void)[] = []
const inputs: ((player: Player, pressed: bigint, released: bigint) => void)[] = []
const restoreds: ((player: Player, error: string | undefined) => void)[] = []
const npcsRestoreds: ((error: string | undefined) => void)[] = []
const damages: ((hit: DamageEvent) => DamageResult | void)[] = []
const damageds: ((hit: DamagedEvent) => void)[] = []
const launches: ((projectile: LaunchEvent) => void)[] = []
const impacts: ((impact: ImpactEvent) => void)[] = []
const landeds: ((landing: LandedEvent) => void)[] = []
const settingChanges: ((player: Player, key: string, value: string) => void)[] = []

/** handlers delivers each event to the handlers the mod registered. */
const handlers: ModHandlers = {
  start(event) {
    for (const handler of starts) {
      handler(event.args)
    }
    // Frames also fade the notices toast shows, so the library always takes them.
    return { frames: true, damage: damages.length !== 0, damaged: damageds.length !== 0 }
  },

  frame(event) {
    tick(event.timeSeconds)
    for (const handler of frames) {
      handler(event)
    }
  },

  command(event) {
    // Split the command line into its name and arguments.
    const line = event.line.trim()
    const space = line.indexOf(' ')
    const handler = commands.get(space < 0 ? line : line.slice(0, space))
    if (!handler) {
      return { claimed: false }
    }

    // Run the handler and claim the command.
    handler(event.player, space < 0 ? '' : line.slice(space + 1).trim())
    return { claimed: true }
  },

  world(event) {
    for (const handler of worlds) {
      handler(event.map)
    }
  },

  uiPress(event) {
    press(event.player, event.node)
  },

  serve(event) {
    const handler = services.get(event.service)
    if (!handler) {
      throw new Error(`the mod serves no service ${event.service}`)
    }
    return { payload: handler(event.method, event.payload) }
  },

  damage(event) {
    let amount = event.amount
    for (const handler of damages) {
      const change = handler({ ...event, amount })
      if (change?.block) {
        return { block: true }
      }
      amount = change?.amount ?? amount
    }
    return amount === event.amount ? {} : { amount }
  },

  damaged(event) {
    for (const handler of damageds) {
      handler(event)
    }
  },

  input(event) {
    for (const handler of inputs) {
      handler(event.player, event.pressed, event.released)
    }
  },

  restored(event) {
    for (const handler of restoreds) {
      handler(event.player, event.error || undefined)
    }
  },

  npcsRestored(event) {
    for (const handler of npcsRestoreds) {
      handler(event.error || undefined)
    }
  },

  launch(event) {
    for (const handler of launches) {
      handler(event)
    }
  },

  impact(event) {
    for (const handler of impacts) {
      handler(event)
    }
  },

  landed(event) {
    for (const handler of landeds) {
      handler(event)
    }
  },

  settingChanged(event) {
    for (const handler of settingChanges) {
      handler(event.player, event.key, event.value)
    }
  },
}

host.event = (call) => serveMod(handlers, call)
