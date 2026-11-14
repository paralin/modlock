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
  type AbilityCastEvent,
  type KillEvent,
  type DamageEvent,
  type ChatEvent,
  type Connection,
  type DamagedEvent,
  type DamageResult,
  type FrameEvent,
  type HealEvent,
  type ImpactEvent,
  type LandedEvent,
  type LaunchEvent,
  type LevelEvent,
  log as logLine,
  type MeleeEvent,
  type ModHandlers,
  type ModifierFiredEvent,
  type ParryEvent,
  type Pawn,
  Player,
  type PlayerJoinedEvent,
  type PlayerLeftEvent,
  serveMod,
} from './host.gen.js'
import { serveEvents } from './host.js'
import { Cast, type Want } from './cast.js'
import { connection, keepPlayers } from './players.js'
import { advancePuppets } from './puppet.js'
import { settleRestore } from './restore.js'
import { advance, after } from './timer.js'
import { change, press, tick } from './ui.js'
export * as vector from './vector.js'

export { Buttons, Layers } from './bits.js'
export { after, every, sequence, type Timer } from './timer.js'
export {
  type CameraMove,
  type CameraPlayOptions,
  cameraPulse,
  type CameraPulseOptions,
  cameraShake,
  type CameraShakeOptions,
  playCamera,
  setOrbitCamera,
} from './camera.js'
export * as curve from './curve.js'
export * as format from './format.js'
export {
  findHero,
  findTitled,
  type Hero,
  heroes,
  heroNamed,
  type Titled,
} from './heroes.js'
export { item, type Item, itemId, items, type Slot } from './items.js'
export {
  Hook,
  type HookAbility,
  hookAbility,
  type HookPlayer,
  type HookRead,
  hookUpgrades,
  type ReadHandle,
} from './hook.js'
export {
  Banner,
  type BannerProps,
  type BoxProps,
  Pill,
  Pips,
  type PipsProps,
  Plate,
  Roster,
  type RosterProps,
  type RosterRow,
  rosterKey,
  rosterRows,
  Score,
  type ScoreProps,
  Tag,
  type TagProps,
  Text,
  type TextProps,
  TopStrip,
  type TopStripProps,
  Action,
  type ActionProps,
  Meter,
  type MeterProps,
} from './hud.js'
export * as theme from './theme.js'
export type { Face, PipShape, TextRole, Tone } from './theme.js'
export * from './host.gen.js'
export { Cast, type CastOptions, type Want } from './cast.js'
export { frameClock } from './frameclock.js'
export { ground, type GroundOptions } from './ground.js'
export {
  RaceGate,
  type RaceGateOptions,
  raceGateResources,
} from './racegate.js'
export { connection, players } from './players.js'
export {
  afterMatch,
  fewestPlayed,
  leaderboard,
  leastUsed,
  nextMatch,
  ratingChange,
  ratingStep,
  settle,
  type Standing,
  Standings,
  startingRating,
} from './rotation.js'
export { bystanderStates, holdBystander } from './bystander.js'
export { Strays, strayGraceSeconds } from './strays.js'
export { type Check, ProbeRun } from './probe.js'
export {
  poseAt,
  type Pose,
  Puppet,
  type PuppetOptions,
  type PuppetState,
  ghostStates,
  ticksPerSecond,
  type Track,
  trackTicks,
} from './puppet.js'
export { type HeroSnapshot, restoreHero } from './restore.js'
export { type ReplayedHit, replayDamage } from './replayed-damage.js'
export { type CatalogEntry, Dropper, dropperButtons } from './dropper.js'
export {
  type BoardPage,
  type BoardRead,
  type BoardRow,
  type BoardWindow,
  type MatchPlace,
  readBoard,
  reportMatch,
  reportScore,
  type ScoreOptions,
} from './scores.js'
export {
  decodeSpot,
  encodeSpot,
  LoadedSpot,
  loadSpot,
  type Placed,
  type Spot,
  type SpotObject,
  triggers,
} from './spot.js'
export {
  type ButtonProps,
  type Child,
  type Component,
  type Edges,
  type ElementProps,
  Fragment,
  type Gradient,
  type HeroLookName,
  type HeroProps,
  hide,
  hideHud,
  cursorFree,
  freeCursor,
  abilityBarPresses,
  type AbilityBarHandler,
  abilityDisplay,
  type AbilityRow,
  type ShownAbility,
  type HudPart,
  type ImageProps,
  type JSX,
  jsx,
  jsxs,
  type LabelProps,
  type LayoutProps,
  type Length,
  type Overflow,
  type PanelProps,
  route,
  type Scaling,
  type Shadow,
  sheet,
  show,
  type Style,
  toast,
  type Transition,
  type UiElement,
  uiSound,
} from './ui.js'

/** LogPart is a value log writes as text. */
export type LogPart = string | number | bigint | boolean | null | undefined

/**
 * log writes one line to the server log under the mod's name: its parts as
 * text, separated by spaces.
 */
export function log(...parts: LogPart[]): void {
  logLine(parts.map((part) => String(part)).join(' '))
}

/**
 * Service answers the host's calls to one service the mod serves: the
 * method's name and payload in, the answer out. A thrown error becomes the
 * call's error.
 */
export type Service = (method: string, payload: Uint8Array) => Uint8Array

/** serve answers the host's calls to service with handler, replacing its earlier handler. */
export function serve(service: string, handler: Service): void {
  services.set(service, { handler })
}

/**
 * command calls handler when a player types /name in chat or their client
 * sends the console command name to the server. The handler runs with the
 * next server frame, before the frame handlers, and receives the text after
 * the name, trimmed of surrounding spaces. Names are lower case and match
 * whatever case the player types. Registering a name again replaces its
 * handler.
 */
export function command(
  name: string,
  handler: (player: Player, args: string) => void,
): void {
  commands.set(name, { handler })
}

/**
 * onStart calls handler once when the server starts the mod, with the
 * arguments that follow -- on the modlock-host command line. The mod starts
 * before any world loads, so find the map's entities in onWorld.
 */
export function onStart(handler: (args: readonly string[]) => void): void {
  starts.push(handler)
}

/**
 * onChat calls handler with each chat line a player writes, commands
 * included, on the next server frame.
 */
export function onChat(handler: (line: ChatEvent) => void): void {
  chats.push(handler)
}

/**
 * onPlayerJoin calls handler once for each player the mod serves when they
 * are ready: a human who finished joining, a bot once added, and at start
 * each player already in the game.
 */
export function onPlayerJoin(
  handler: (joined: PlayerJoinedEvent) => void,
): void {
  joins.push(handler)
}

/**
 * onPlayerLeave calls handler once for each joined player who leaves the mod,
 * with the reason the server dropped them when it gave one. A player who
 * disconnects still has their slot and hero during the call. A player the
 * mod stops serving leaves too, with disconnected false.
 */
export function onPlayerLeave(handler: (left: PlayerLeftEvent) => void): void {
  leaves.push(handler)
}

/**
 * onSpawn calls handler each time a player's hero enters play: their first
 * hero after joining, and each respawn. The
 * player has a hero to move, equip and read from then on, which onPlayerJoin
 * does not promise. The library hears it from the game's PLAYER_RESPAWNED
 * event, before the frame handlers run.
 */
export function onSpawn(handler: (player: Player, pawn: Pawn) => void): void {
  spawns.push(handler)
}

/** respawnedEvent is the game's modifier event for a hero entering play. */
const respawnedEvent = 'PLAYER_RESPAWNED'

/** frameCount counts frames, so spawnedAt can tell one frame's events apart. */
let frameCount = 0

/**
 * spawnedAt maps each slot to the frame the spawn handlers last heard of its
 * hero; the game repeats the event as a player's hero is built.
 */
const spawnedAt = new Map<number, number>()

/** Seating is the hero and team seat gives one player. */
export interface Seating {
  /**
   * hero is the hero's name as players know it or as the game does, such as
   * 'haze' or 'hero_haze', its id, or 'any' for the hero the player has or
   * else a random one. 'any' is the default.
   */
  readonly hero?: string | number
  /**
   * team is the team, 2 or 3. Without one the player keeps their hero's team
   * or joins the side with fewer heroes.
   */
  readonly team?: number
}

/**
 * seated holds what each seated player wants by slot, with the generation of
 * the connection it was asked for, so a wish never passes to the next player
 * in the slot.
 */
const seated = new Map<number, { generation: number; want: Want }>()

/** respawnDelay is the seconds a fallen hero waits, or undefined for the game's own time. */
let respawnDelay: number | undefined

/** unseated is the want of a player the mod did not seat: only the respawn delay. */
let unseated: Want = {}

/** seating is the Cast that seats players, once the mod uses seating. */
let seating: Cast<Connection> | undefined

/**
 * startSeating makes the seating Cast and runs it with the mod's frames. Bots
 * are members, so the mod can seat one and the respawn delay brings theirs
 * back too. It starts with the first seat or respawn delay, so a mod that uses
 * neither pays nothing.
 */
function startSeating(): void {
  if (seating) {
    return
  }
  const cast = new Cast<Connection>({
    join: (connection) => connection,
    leave(member) {
      if (seated.get(member.player.slot)?.generation === member.generation) {
        seated.delete(member.player.slot)
      }
    },
    want(member) {
      const held = seated.get(member.player.slot)
      return held?.generation === member.generation ? held.want : unseated
    },
    bots: 'join',
  })
  seating = cast
  onFrame(({ timeSeconds }) => cast.frame(timeSeconds))
}

/** wantOf returns a seating with the respawn delay as the seating Cast's want. */
function wantOf(seating: Seating): Want {
  return {
    hero: seating.hero ?? 'any',
    ...(seating.team === undefined ? {} : { team: seating.team }),
    ...(respawnDelay === undefined ? {} : { respawn: respawnDelay }),
  }
}

// The library adds the methods below to the generated Player.
declare module './host.gen.js' {
  interface Player {
    /**
     * connection returns the player's connection: their name, Steam ID,
     * whether they are a bot and whether they finished joining. It returns
     * undefined when nobody holds the slot.
     */
    connection(): Connection | undefined
    /**
     * seat gives the player a hero on a team, skipping the game's team and
     * hero screens. Call it as the player joins:
     *
     *     onPlayerJoin(({ connection }) => connection.player.seat({ hero: 'haze', team: 2 }))
     *
     * The hero appears on a later frame, and onSpawn hears of it. The library
     * asks the game again until the player has the seat, and once more after
     * the player changes hero, so a call before the game is ready is not
     * lost. Seating the player again replaces the seat, and it ends when they
     * leave. It returns false when nobody holds the player's slot.
     */
    seat(seating?: Seating): boolean
  }
}

Player.prototype.connection = function (this: Player): Connection | undefined {
  return connection(this)
}

Player.prototype.seat = function (
  this: Player,
  seating: Seating = {},
): boolean {
  // Find the connection the seat is for.
  const held = connection(this)
  if (!held) {
    return false
  }

  // Keep the seat for the seating Cast to ask for.
  seated.set(this.slot, { generation: held.generation, want: wantOf(seating) })
  startSeating()
  return true
}

/**
 * setRespawnDelay brings every fallen hero back after seconds, in place of
 * the game's own respawn time. Without seconds it leaves fallen heroes to the
 * game again.
 */
export function setRespawnDelay(seconds?: number): void {
  respawnDelay = seconds
  unseated = seconds === undefined ? {} : { respawn: seconds }
  for (const held of seated.values()) {
    const { respawn: _, ...want } = held.want
    held.want = { ...want, ...unseated }
  }
  startSeating()
}

/** respawned tells the spawn handlers that the player's hero entered play. */
function respawned(player: Player): void {
  // Skip a placeholder pawn, which has hero 0, and a hero already told of this frame.
  const pawn = player.slot < 0 ? undefined : player.pawn()
  if (pawn === undefined || pawn.hero === 0 || pawn.health <= 0) {
    return
  }
  if (spawnedAt.get(player.slot) === frameCount) {
    return
  }
  spawnedAt.set(player.slot, frameCount)
  for (const handler of spawns) {
    handler(player, pawn)
  }
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
export function onInput(
  handler: (player: Player, pressed: bigint, released: bigint) => void,
): void {
  inputs.push(handler)
}

/**
 * onRestored calls handler when a player's Player.restoreHero ends: error is
 * undefined once the target held for a second, or says why it did not.
 */
export function onRestored(
  handler: (player: Player, error: string | undefined) => void,
): void {
  restoreds.push(handler)
}

/**
 * onNpcsRestored calls handler when restoreNpcs ends: error is undefined once
 * the map holds every target, or says why it does not.
 */
export function onNpcsRestored(
  handler: (error: string | undefined) => void,
): void {
  npcsRestoreds.push(handler)
}

/**
 * onDamage calls handler before each hit lands, so it can block the hit or
 * change its damage. Later handlers see the earlier ones' amount.
 */
export function onDamage(
  handler: (hit: DamageEvent) => DamageResult | void,
): void {
  damages.push(handler)
}

/** onDamaged calls handler on the frame after each hit lands. */
export function onDamaged(handler: (hit: DamagedEvent) => void): void {
  damageds.push(handler)
}

/**
 * onKill calls handler after each hit that kills a hero or a unit, before the
 * next frame. The kill names both entities and the player of each: a slot of
 * -1 is a unit's, or a hazard's for the attacker.
 */
export function onKill(handler: (kill: KillEvent) => void): void {
  kills.push(handler)
}

/**
 * onAbilityCast calls handler after each ability the game executes, before
 * the next frame: a hero's abilities and the items a player activates, at
 * the moment the cast takes effect.
 */
export function onAbilityCast(handler: (cast: AbilityCastEvent) => void): void {
  abilityCasts.push(handler)
}

/**
 * onHeal calls handler after each heal, before the next frame: health a hero
 * or unit regained at once from an ability, an item or lifesteal. Regeneration
 * calls nothing, including what abilities such as Siphon Life restore as
 * regeneration, and neither do the mod's own heal and a heal at full health.
 * The heal names the entity healed, the healer and ability when the game
 * names them, and the player of each: a slot of -1 is a unit's, or no one's.
 */
export function onHeal(handler: (heal: HealEvent) => void): void {
  heals.push(handler)
}

/**
 * onLevelChange calls handler as each hero reaches a new level, before the
 * next frame.
 */
export function onLevelChange(handler: (change: LevelEvent) => void): void {
  levels.push(handler)
}

/**
 * onMelee calls handler after each melee swing, before the next frame, with
 * whether it was heavy and the entities it struck, none for a miss.
 */
export function onMelee(handler: (swing: MeleeEvent) => void): void {
  melees.push(handler)
}

/**
 * onParry calls handler after each parry that catches a melee swing, before
 * the next frame, naming the hero that parried and the attacker it caught.
 */
export function onParry(handler: (parry: ParryEvent) => void): void {
  parries.push(handler)
}

/**
 * onModifierEvent calls handler each time the game fires the modifier event
 * with this EModifierEvent name, such as ABILITY_CAST_STARTED, PARRY_SUCCESS
 * or PLAYER_RESPAWNED, before the next frame. Register before the mod starts.
 * A name the running game lacks is logged once and never fires.
 */
export function onModifierEvent(
  name: string,
  handler: (event: ModifierFiredEvent) => void,
): void {
  const handlers = modifierEvents.get(name) ?? []
  handlers.push(handler)
  modifierEvents.set(name, handlers)
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

// Maps hold each handler in a record, since scriptc has no Map slot for a
// function.

/** commands maps each registered command name to its handler. */
const commands = new Map<
  string,
  { readonly handler: (player: Player, args: string) => void }
>()

/** services maps each service the mod serves to its handler. */
const services = new Map<string, { readonly handler: Service }>()

// Each list runs in registration order.
const starts: ((args: readonly string[]) => void)[] = []
const frames: ((frame: FrameEvent) => void)[] = []
const chats: ((line: ChatEvent) => void)[] = []
const joins: ((joined: PlayerJoinedEvent) => void)[] = []
const leaves: ((left: PlayerLeftEvent) => void)[] = []
const spawns: ((player: Player, pawn: Pawn) => void)[] = []
const worlds: ((map: string) => void)[] = []
const inputs: ((player: Player, pressed: bigint, released: bigint) => void)[] =
  []
const restoreds: ((player: Player, error: string | undefined) => void)[] = []
const npcsRestoreds: ((error: string | undefined) => void)[] = []
const damages: ((hit: DamageEvent) => DamageResult | void)[] = []
const damageds: ((hit: DamagedEvent) => void)[] = []
const kills: ((kill: KillEvent) => void)[] = []
const abilityCasts: ((cast: AbilityCastEvent) => void)[] = []
const heals: ((heal: HealEvent) => void)[] = []
const levels: ((change: LevelEvent) => void)[] = []
const melees: ((swing: MeleeEvent) => void)[] = []
const parries: ((parry: ParryEvent) => void)[] = []
const modifierEvents = new Map<
  string,
  ((event: ModifierFiredEvent) => void)[]
>()
const launches: ((projectile: LaunchEvent) => void)[] = []
const impacts: ((impact: ImpactEvent) => void)[] = []
const landeds: ((landing: LandedEvent) => void)[] = []
const settingChanges: ((player: Player, key: string, value: string) => void)[] =
  []

/** handlers delivers each event to the handlers the mod registered. */
const handlers: ModHandlers = {
  start(event) {
    for (const handler of starts) {
      handler(event.args)
    }
    // Frames also fade the notices toast shows, so the library always takes them.
    return {
      frames: true,
      damage: damages.length !== 0,
      damaged: damageds.length !== 0,
      chat: chats.length !== 0,
      joins: joins.length !== 0 || leaves.length !== 0,
      casts: abilityCasts.length !== 0,
      kills: kills.length !== 0,
      heals: heals.length !== 0,
      levels: levels.length !== 0,
      melees: melees.length !== 0,
      parries: parries.length !== 0,
      modifierEvents: [
        ...new Set([
          ...modifierEvents.keys(),
          ...(spawns.length !== 0 ? [respawnedEvent] : []),
        ]),
      ],
    }
  },

  frame(event) {
    // Advance the library's own clocks before the mod's handlers run.
    tick(event.timeSeconds)
    advance(event.timeSeconds)
    advancePuppets(event.timeSeconds)

    // Count the frame, then tell the mod.
    frameCount++
    for (const handler of frames) {
      handler(event)
    }
  },

  command(event) {
    // Split the command line into its name and arguments.
    const line = event.line.trim()
    const space = line.indexOf(' ')
    const registered = commands.get(space < 0 ? line : line.slice(0, space))
    if (!registered) {
      return { claimed: false }
    }

    // Claim the command and run its handler with the next frame. The server
    // reads a client's commands under a CPU budget and drops the client when
    // one handler spends it; slow work in a frame only delays that frame.
    const args = space < 0 ? '' : line.slice(space + 1).trim()
    after(0, () => registered.handler(event.player, args))
    return { claimed: true }
  },

  chat(event) {
    // A line arrives under the same CPU budget as a command, so its handlers
    // run with the next frame.
    after(0, () => {
      for (const handler of chats) {
        handler(event)
      }
    })
  },

  world(event) {
    for (const handler of worlds) {
      handler(event.map)
    }
  },

  uiPress(event) {
    // A press arrives under the same CPU budget as a command, so its handler
    // also runs with the next frame.
    after(0, () => press(event.player, event.node))
  },

  uiInput(event) {
    // A change runs with the next frame, as a press does.
    after(0, () => change(event.player, event.node, event.value))
  },

  serve(event) {
    const registered = services.get(event.service)
    if (!registered) {
      throw new Error(`the mod serves no service ${event.service}`)
    }
    return { payload: registered.handler(event.method, event.payload) }
  },

  // scriptc types an unannotated method that returns literals of different
  // shapes by those literals rather than by ModHandlers, and the call throws.
  damage(event): DamageResult {
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

  killed(event) {
    for (const handler of kills) {
      handler(event)
    }
  },

  abilityCast(event) {
    for (const handler of abilityCasts) {
      handler(event)
    }
  },

  healed(event) {
    for (const handler of heals) {
      handler(event)
    }
  },

  levelChanged(event) {
    for (const handler of levels) {
      handler(event)
    }
  },

  melee(event) {
    for (const handler of melees) {
      handler(event)
    }
  },

  parried(event) {
    for (const handler of parries) {
      handler(event)
    }
  },

  modifierFired(event) {
    if (event.name === respawnedEvent && spawns.length !== 0) {
      respawned(event.player)
    }
    for (const handler of modifierEvents.get(event.name) ?? []) {
      handler(event)
    }
  },

  input(event) {
    for (const handler of inputs) {
      handler(event.player, event.pressed, event.released)
    }
  },

  restored(event) {
    settleRestore(event.player, event.error || undefined)
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

  playersChanged(event) {
    keepPlayers(event.players)
  },

  // A join and a leave run at once, so the leaving player's hero still reads.
  playerJoined(event) {
    for (const handler of joins) {
      handler(event)
    }
  },

  playerLeft(event) {
    for (const handler of leaves) {
      handler(event)
    }
  },
}

serveEvents((call) => serveMod(handlers, call))
