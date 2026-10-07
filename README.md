# Modlock

**Modlock** is the best tool for hand-writing the logic of [Deadlock] custom
game modes. You write a mod in TypeScript, Luau or Python; Modlock builds it
for WebAssembly, and the game server runs it in a secure sandbox. Every mod
speaks to the game through one common protobuf schema, so each language sees
the same events and calls. Mods deploy to [hyperline.gg], the default backend and
marketplace for custom games.

[Deadlock]: https://store.steampowered.com/app/1422450/Deadlock/
[hyperline.gg]: https://hyperline.gg

```ts
import { command } from 'modlock'

command('hello', (player) => {
  player.chat('Hello from TypeScript!')
})
```

> **Early development.** APIs change without notice.
> [TypeScript, JavaScript](#the-typescript-library),
> [Luau](#writing-a-mod-in-luau), [Python](#writing-a-mod-in-python) and
> [Go](#writing-a-mod-in-go) mods, the `modlock` command line and
> `modlock publish` work today.

## Why WebAssembly

- **Safe to share.** A mod runs in a [Wasmtime] sandbox inside the server. It
  has no files, network or environment, only the calls the schema offers. A
  crash, an endless loop or a runaway allocation stops that mod with a log line
  and leaves the match running.
- **Any language.** Anything that compiles to WebAssembly can be a mod. Each
  language gets a small library over the generated protobuf types.
- **Built once.** A mod is a portable `.wasm` file. It does not depend on the
  compiler or source revision of the server that runs it.
- **Fast.** Wasmtime compiles mods to machine code with Cranelift, and the
  server calls them in the game frame, so a mod can decide whether to claim a
  command or change a frame as it happens.

[Wasmtime]: https://wasmtime.dev

## Getting started

Download `modlock` for your system from the
[releases](https://github.com/paralin/modlock/releases). On macOS and Linux,
install it on your `PATH` as `modlock`, using the name of your download:

```sh
sudo install -m 755 modlock-darwin-arm64 /usr/local/bin/modlock
```

macOS refuses to open an unsigned program that a browser downloaded, so on a
Mac clear the download's quarantine first with `xattr -c modlock-darwin-arm64`.

On Windows it runs the game server directly; on Linux it runs it through
Steam's Proton. It fetches the server that matches its release the first time.
Script mods need nothing else installed; a Go mod needs
[Go](https://go.dev/dl/).

```sh
modlock new my-mod
cd my-mod
modlock dev
```

`modlock new` writes a TypeScript project:

| File            | Holds                                                                |
| --------------- | -------------------------------------------------------------------- |
| `mod.json`      | The [manifest](#the-manifest): the mod's slug, name and version.     |
| `main.ts`       | The mod. It answers `/hello` in chat.                                |
| `tsconfig.json` | The compiler options your editor uses to check the code as you type. |

Pass `--language javascript`, `luau`, `python` or `go` for another language.
Complete game modes to start from include
[`arena`](examples/arena) in TypeScript, [`bounty`](examples/bounty) in Luau
and [`race`](examples/race) in Go.

`modlock dev` builds the mod, starts a local server with it, and launches
Deadlock through Steam to join. Each time you save, it rebuilds the mod and
swaps it into the running server. A build that fails prints its errors and
keeps the previous build running. The terminal shows the mod's log, reloads
and player joins; the server console goes to `build/server.log`.

### Without the game

On macOS, or on a computer without Deadlock, `modlock dev` runs the mod in the
sandbox: the same WebAssembly runtime and limits, with a stand-in player who
has no hero. Type `/hello` and press Enter to send it as that player. The
terminal shows the mod's log and the chat, toasts and announcements it sends.
Calls that need the game, such as reading a hero's position, return nothing.
`--sandbox` uses the sandbox even where the game is installed.

With `--json`, a program such as an editor drives the sandbox: it sends each
line of input as a `modlock.cli.Input` (a command or a button press) and draws
the interface from the events it reads back.

### Commands

| Command                 | Effect                                                              |
| ----------------------- | ------------------------------------------------------------------- |
| `modlock new DIR`       | Create a mod project in `DIR`.                                      |
| `modlock build`         | Check the mod and write the built mod to `build/`.                  |
| `modlock dev`           | Run the mod in a local server, join it, and reload it on each save. |
| `modlock play [MOD...]` | Run built mods in a local server and join it.                       |
| `modlock publish`       | Build and check the mod, then [publish it](#publishing).            |

`dev` and `play` fall back to the sandbox when the game is missing. They also take these options:

| Option        | Effect                                                    |
| ------------- | --------------------------------------------------------- |
| `--no-game`   | Run the server without launching Deadlock.                |
| `--port PORT` | Serve on another UDP port.                                |
| `--arg VALUE` | Pass `VALUE` to the mods' `onStart` handlers; repeatable. |
| `--sandbox`   | Run in the sandbox even when the game is installed.       |

| Variable         | Use it when                                                                          |
| ---------------- | ------------------------------------------------------------------------------------ |
| `DEADLOCK_DIR`   | Deadlock is installed outside the Steam libraries.                                   |
| `MODLOCK_HOST`   | You built `modlock-host.exe` yourself. The sandbox reads the interpreters beside it. |
| `MODLOCK_PROTON` | You want a particular Proton installation.                                           |

## Tutorial

Each step adds a few lines to the project `modlock new` wrote, and you can
try each one as soon as it saves. Steps 1 to 4 go in `main.ts`; step 5 adds a
module beside it. Together they build a small brawl with a countdown, a score,
a HUD and a sign. The [examples](#examples) carry the same ideas into full
game modes.

### 1. Answer a command

`command` runs a handler when a player types `/name` in chat. The handler gets
the player and the text after the command. `toast` shows the player a short
notice that fades.

```ts
import { command, toast } from 'modlock'

command('roll', (player, args) => {
  const sides = Number(args) || 6
  const roll = 1 + Math.floor(Math.random() * sides)
  toast(player, `You rolled ${roll} out of ${sides}.`)
})
```

Type `/roll 20`.

### 2. Read the hero

`player.pawn()` returns the player's hero as it is this frame: health, team,
position, aim and souls. It returns `undefined` until the hero spawns, and a
hero with no health left is dead.

```ts
import { command, toast } from 'modlock'

command('where', (player) => {
  const pawn = player.pawn()
  if (pawn === undefined || pawn.health <= 0) {
    toast(player, 'You have no living hero.')
    return
  }
  const { x, y } = pawn.position
  toast(
    player,
    `You stand at ${x.toFixed(0)}, ${y.toFixed(0)} with ${pawn.health} health.`,
  )
})
```

### 3. Count down on the frame

The server calls `onFrame` once per frame. The frame rate varies, so never
count frames: store when something should happen and compare it with the game
clock in `frame.timeSeconds`. `players()` lists who is connected now, bots
included.

```ts
import { command, onFrame, players } from 'modlock'

let now = 0
let liveAt: number | undefined

onFrame((frame) => {
  now = frame.timeSeconds
  if (liveAt === undefined || now < liveAt) {
    return
  }
  liveAt = undefined
  for (const { player } of players()) {
    player.freeze(false)
    player.announce('Fight!', 'The round is live.')
  }
})

command('start', () => {
  liveAt = now + 5
  for (const { player } of players()) {
    player.freeze(true)
  }
})
```

`/start` freezes every hero for five seconds, then announces the round.

### 4. Decide the fights

`onDamage` sees each hit before it lands. Return `{ block: true }` to stop it,
or `{ amount }` to change it. `onDamaged` reports each hit after it lands,
with the victim's health before and the health it lost, so a lethal hit is one
that took all of it. Hits refer to heroes by entity, the same number as
`pawn.entity`.

```ts
import { onDamage, onDamaged, type Player, players } from 'modlock'

let live = false
const defeats = new Map<number, number>()

// Nobody gets hurt until the round is live.
onDamage(() => (live ? undefined : { block: true }))

// A lethal hit scores for the player who landed it.
onDamaged((hit) => {
  const scorer = playerOf(hit.attacker)
  if (hit.healthLost < hit.healthBefore || scorer === undefined) {
    return
  }
  defeats.set(scorer.slot, (defeats.get(scorer.slot) ?? 0) + 1)
})

function playerOf(entity: number): Player | undefined {
  return players().find(({ player }) => player.pawn()?.entity === entity)
    ?.player
}
```

Set `live = true` in step 3's frame handler where the round goes live. This
map keys scores by slot. When
players come and go, key them by slot and `generation` instead: the generation
changes when someone new takes the slot.

### 5. Show a HUD

An interface is JSX in a `.tsx` module, built from four elements (`panel`,
`label`, `image` and `button`) and a fixed set of style properties. Put the
HUD in `hud.tsx`:

```tsx
import { type Player, show } from 'modlock'

export function drawScore(player: Player, defeats: number): void {
  show(player, <Score defeats={defeats} />)
}

function Score(props: { defeats: number }) {
  return (
    <panel
      style={{
        horizontalAlign: 'right',
        verticalAlign: 'top',
        margin: [80, 24],
        padding: 10,
        background: '#101820d0',
        borderRadius: 6,
      }}
    >
      <label style={{ fontSize: 24, bold: true }}>
        {props.defeats} defeats
      </label>
    </panel>
  )
}
```

and draw it from `main.ts` on every frame:

```ts
import { drawScore } from './hud'

onFrame(() => {
  for (const { player } of players()) {
    drawScore(player, defeats.get(player.slot) ?? 0)
  }
})
```

`show` sends only what changed since its last call, so redraw every player's
HUD from your state each frame instead of tracking what to update. A
`<button onPress={(player) => ready(player)}>` calls back into the mod when a
player presses it.

### 6. Place things in the world

`createText`, `createModel` and `createParticle` place floating text, props
and effects. Each returns an object you can move, change or remove. A new map
removes everything, so forget your objects in `onWorld`, which runs each time
a world loads.

```ts
import { command, createText, onWorld, type WorldObject } from 'modlock'

let sign: WorldObject | undefined

// A new world has removed the sign.
onWorld(() => {
  sign = undefined
})

command('sign', (player, args) => {
  const pawn = player.pawn()
  if (pawn === undefined) {
    return
  }
  const { x, y, z } = pawn.position
  sign?.remove()
  sign = createText({
    text: args || 'Hello!',
    position: { x, y, z: z + 120 },
    faceCamera: true,
  })
})
```

Call `precache` from `onStart` with the heroes and models your mode uses, so
they load with the world.

### 7. Let players choose, and keep score

A setting is a choice each player keeps between matches, such as where the
score sits. A metric is a total the mod keeps for each player, such as their
defeats. Declare both in `mod.json`:

```json
"settings": [
  {
    "key": "corner",
    "label": "Score corner",
    "kind": "KIND_CHOICE",
    "choices": [
      { "value": "right", "label": "Top right" },
      { "value": "left", "label": "Top left" }
    ]
  }
],
"metrics": [{ "name": "brawl.defeats", "kind": "KIND_COUNT" }]
```

Then read the player's setting when you draw the HUD, and add to the metric
when they score:

```ts
const corner = player.setting('corner') === 'left' ? 'left' : 'right'
scorer.addMetric('brawl.defeats', 1)
```

[The manifest](#the-manifest) lists every kind of setting and metric.

### 8. Split it into modules

As the mode grows, keep `main.ts` a short list of wiring and keep the game's
state in one object the handlers call into. Split the rest by concern, such
as the match rules and the HUD. `modlock build` bundles every module `main.ts`
imports and checks their types together.

```ts
import { command, onDamage, onFrame, onStart, onWorld } from 'modlock'

import { drawScoreboards } from './hud'
import { Match } from './match'

const match = new Match()

onStart(() => match.start())
onWorld(() => match.world())
onDamage((hit) => match.damage(hit))
onFrame((frame) => {
  match.frame(frame.timeSeconds)
  drawScoreboards(match)
})
command('ready', (player) => match.ready(player))
```

[`examples/arena`](examples/arena) is laid out this way. Each event runs
within a time budget, and a mod that overruns it stops for the rest of the
match, so keep each frame's work small.

### 9. Test a whole round

The sandbox runs commands and interfaces in seconds; use it while you write
the rules. To test a whole round in the game, let the mod test itself. Started
with `modlock play --arg probe`, it seats bots with `addBot`, plays the round,
checks the outcome, logs PASS or FAIL and quits with `serverCommand('quit')`.
[`examples/arena`](examples/arena/probe.ts) does this.

When the mode plays well, [publish it](#publishing).

## Examples

| Example                                 | Shows                                                               |
| --------------------------------------- | ------------------------------------------------------------------- |
| [`hello-ts`](examples/hello-ts)         | Commands, a JSX menu with buttons, moving text, a setting, metrics. |
| [`arena`](examples/arena)               | A free-for-all: ready-up, countdown, kills, bots, a self-test.      |
| [`hill`](examples/hill)                 | King of the hill: a ring of world text, team scores, progress bars. |
| [`drill`](examples/drill)               | An aim drill: target bots, headshots, a length setting, best score. |
| [`dropper`](examples/dropper)           | Building a spot of solid objects while playing.                     |
| [`bounty`](examples/bounty)             | A Luau free-for-all: a bounty marker, damage hooks, flares, bots.   |
| [`race`](examples/race)                 | A Go checkpoint race: a marked course, laps, resets, unit tests.    |
| [`hello-luau`](examples/hello-luau)     | The smallest mod in Luau.                                           |
| [`hello-python`](examples/hello-python) | The smallest mod in Python.                                         |
| [`hello-go`](examples/hello-go)         | The smallest mod in Go.                                             |
| [`hello`](examples/hello)               | A native C++ plugin.                                                |

## Publishing

`modlock publish` builds the mod and runs the check every host runs before it
loads a mod: the module may import only the Modlock and WASI functions, and it
must start and answer its first event within the time and memory limits. A mod
that passes goes to the destination `--to` selects:

| `--to`      | Effect                                                                                           |
| ----------- | ------------------------------------------------------------------------------------------------ |
| `hyperline` | The default. Upload the mod to [hyperline.gg](https://hyperline.gg).                             |
| `archive`   | Write `SLUG-VERSION.zip` to `--out`: the mod, `modlock.exe` and `Play.cmd` to run it on Windows. |
| `github`    | Attach that zip to the GitHub release `SLUG-vVERSION`, creating the release if needed.           |

The first upload to hyperline.gg opens the browser to sign in and saves the
session in your user configuration directory. `--origin` points it at another
service with the same API. The `github` destination uses the
[`gh`](https://cli.github.com) command line and its sign-in.

A release carries the built `mod.json` and its entry. Hyperline publishes each
version once, so raise `version` in `mod.json` before publishing again.
`--notes` says what the release changed: hyperline.gg shows the notes on the
game's page, and the `github` destination uses them for a new release.

## The TypeScript library

A TypeScript or JavaScript mod imports `modlock` and registers its handlers
when it loads. `main.ts`, or `main.js`, is the entry.

`modlock build` installs the `modlock` library into `node_modules/`, checks the
types with the TypeScript native compiler (downloaded the first time), and
bundles the mod into `build/mod.js`. A JavaScript mod is checked from its
JSDoc. No Node.js or npm is needed. The server runs the bundle on
[QuickJS](https://github.com/quickjs-ng/quickjs), itself compiled to
WebAssembly, so a script mod has the same sandbox and limits as a compiled one.

Every call is generated from the `Host` service in
[`proto/modlock/wasm.proto`](proto/modlock/wasm.proto), which documents each
one. A call that fails logs the failure and returns `false` or `undefined`; it
never stops the mod. Everything a mod places leaves with the world. Stopping or
reloading the mod also removes it and releases frozen heroes, held modifier
states and input.

### Events

| Call                                                | Runs the handler                                              |
| --------------------------------------------------- | ------------------------------------------------------------- |
| `command(name, handler)`                            | When a player types `/name` in chat.                          |
| `onStart(handler)`                                  | When the server starts the mod, with its `--arg` values.      |
| `onWorld(handler)`                                  | With the map's name each time a world loads.                  |
| `onFrame(handler)`                                  | Once per server frame.                                        |
| `onDamage(handler)`, `onDamaged(handler)`           | Before each hit lands, to block or change it; after it lands. |
| `onLanded(handler)`                                 | When a hero lands under the manifest's movement model.        |
| `onSettingChanged(handler)`                         | When a player changes a setting outside the mod.              |
| `watchProjectiles(options)`, `onLaunch`, `onImpact` | On a watched projectile's first frame, and on its impact.     |

### Players and heroes

| Call                                                                                   | Effect                                                          |
| -------------------------------------------------------------------------------------- | --------------------------------------------------------------- |
| `players()`                                                                            | List the connected players and bots.                            |
| `player.pawn()`                                                                        | Read the hero: health, team, position, aim and stamina.         |
| `player.chat`, `centerText`, `announce`                                                | Show text to one player.                                        |
| `player.selectHero`, `respawn`, `teleport`, `setVelocity`, `freeze`, `kill`            | Move the hero through the match.                                |
| `player.spectate()`                                                                    | Move the player to the spectators, without a hero.              |
| `player.clearItems`, `giveItem`, `restoreStamina`, `refreshAbility`                    | Change what the hero carries.                                   |
| `player.abilities()`, `setAbility`, `replaceAbility`, `holdModifier`                   | Read or change the hero's abilities and modifiers.              |
| `player.setting`, `setSetting`, `settingOn(player, key)`, `settingNumber(player, key)` | Read or change the player's settings.                           |
| `player.addMetric(name, value, label?)`                                                | Add to the player's total of a metric.                          |
| `player.watchMovement(true)`                                                           | Add the hero's movement, landings and wall jumps to each frame. |
| `addBot(options)`, `player.removeBot()`                                                | Add or remove a bot player.                                     |

### Input

| Call                                                | Effect                                                  |
| --------------------------------------------------- | ------------------------------------------------------- |
| `blockInput(buttons)`, `player.blockInput(buttons)` | Withhold buttons from every hero, or from one.          |
| `player.press(buttons)`, `player.buttons()`         | Press buttons for a hero, or read the buttons it holds. |
| `remapInput(from, to, repeat)`                      | Make one button act as another for every hero.          |

### The world

| Call                                                     | Effect                                                              |
| -------------------------------------------------------- | ------------------------------------------------------------------- |
| `createText`, `createModel`, `createParticle`            | Place text, a model or an effect; move, change or remove it later.  |
| `precache({ heroes, resources })`                        | Load heroes and resources with the next world.                      |
| `serverCommand(line)`                                    | Run a line at the server console.                                   |
| `moveEntity`, `emitSound`                                | Move an entity and set its velocity, or play a sound on it.         |
| `modifierState(entity, state)`, `holdModifierState(...)` | Test a modifier state on an entity, or hold one on it.              |
| `readField`, `writeField`, `entityClass`                 | Read or write any schema field of a live entity, or name its class. |
| `loadSpot(spot)`, `encodeSpot`, `decodeSpot`             | Place a [spot](#spots) on the running map, or save it.              |
| `new Dropper(catalog, spot)`                             | Let players build a spot while they play.                           |

### Entity fields

A mod reads and writes any schema field of a live entity by its class and
field names, the way the game declares them:

```ts
import { readField, writeField } from 'modlock'

const health = readField(pawn.entity, 'CBaseEntity', 'm_iHealth', 'int32')
writeField(pawn.entity, 'CBaseEntity', 'm_iHealth', 'int32', 500)
```

A write changes the field on the server and sends it to the players. The
host checks that the entity is of the named class or one derived from it, and
refuses one that is not, such as a trooper named as a `CCitadelPlayerPawn`,
with a message naming both classes and the entity unchanged.
`entityClass(entity)` returns an entity's class, such as `CCitadelPlayerPawn`,
and its designer name, such as `player`.

Some fields matter only while an entity spawns, such as a unit's
`m_iInitialTeamNum`. `spawnNpc` writes those before the unit spawns:

```ts
spawnNpc({
  className: 'npc_trooper_boss',
  unit: 'npc_boss_tier1',
  team: 2,
  position,
  fields: [
    {
      className: 'CBaseEntity',
      field: 'm_iInitialTeamNum',
      type: 'int32',
      value: 2,
    },
  ],
})
```

`modlock/entities` has a typed class for each server entity class, with a
getter and, for each field but a string, a setter:

```ts
import { CCitadelPlayerPawn } from 'modlock/entities'

const hero = new CCitadelPlayerPawn(pawn.entity)
hero.m_iHealth = (hero.m_iHealth ?? 0) + 50
```

A bundle keeps only the classes it uses. The game dump in
[`data/dump`](data/dump) lists every class, its base and its fields in
`schemas.json`; the `server` module holds the entity classes.

### Entities and inputs

`createEntity` creates any entity the server knows by its designer name, with
the spawn key values a map would give it and fields written before it spawns.
`fireInput` sends an entity an input, as a map's output would, and
`removeEntity` removes one the mod created. The world removes a mod's entities
when it ends, and so does stopping the mod.

```ts
import { createEntity, fireInput, removeEntity } from 'modlock'

const guardian = createEntity({
  designerName: 'npc_trooper_boss',
  subclass: 'npc_boss_tier1',
  team: 3,
  position,
  keyValues: [{ key: 'rendercolor', value: { text: '255 0 0' } }],
})
fireInput(guardian, 'Alpha', { integer: 128 })
fireInput(guardian, 'Color', { color: 0x00ff00ff })
fireInput(guardian, 'DisableShadow')
removeEntity(guardian)
```

An NPC needs its subclass, the game data entry it reads while it spawns. An
input reads its value as the type it takes and does not convert text: `Alpha`
takes an integer, `Color` a color as `0xRRGGBBAA`. `entities.json` in the game
dump lists each input and the type of its value. A designer name the server
lacks, or an input the entity lacks, fails that call alone.

`create` in `modlock/entities` types the same call by designer name: it takes
only that entity's spawn keys, each as the type the entity reads, and returns
the entity as its class, with a method per input:

```ts
import { create } from 'modlock/entities'

const guardian = create('npc_trooper_boss', {
  subclass: 'npc_boss_tier1',
  team: 3,
  position,
  keys: { rendercolor: 0xff0000ff },
})
guardian?.inputAlpha(128)
guardian?.inputDisableShadow()
```

### Interfaces

| Call                                    | Effect                                                   |
| --------------------------------------- | -------------------------------------------------------- |
| `show(player, element)`, `hide(player)` | Show a player an interface written in JSX, or remove it. |
| `toast(player, text, seconds?)`         | Show a player a short notice that fades.                 |

An interface is built from `panel`, `label`, `image` and `button`. Its style
properties are `width`, `height`, `flow`, `horizontalAlign`, `verticalAlign`,
`margin`, `padding`, `background`, `color`, `fontSize`, `bold`, `textAlign`,
`borderRadius` and `opacity`.

The interface travels as data, the typed tree in
[`proto/modlock/ui.proto`](proto/modlock/ui.proto). The player's game draws it
with stock panels and runs no mod code, and a button press comes back to its
`onPress`. The host clears a player's interface when the player leaves or the
mod stops.

The game draws interfaces with the renderer in [`panorama/`](panorama):

- The layout `panorama/layout/modlock/ui.xml` and its script. Each release
  publishes the script in `modlock-library.tar.gz` as `ui.js`, for
  `scripts/modlock/ui.js`; `go run ./cmd/modlock-library <archive>` builds it
  from a checkout.
- A client content package loads the layout into a panel that stays loaded
  through matches, and the renderer draws over the HUD.
- `modlock dev` and `modlock play` serve the local player's interfaces on
  `127.0.0.1:4320` (`--ui-port`, 0 for none). Deadlock's HTML panels load only
  https pages, so the renderer reads them through a relay page at the https
  origin in `--ui-relay`, `https://hyperline.gg` by default. The controller
  answers only that origin's page in the game's own browser.
- A press runs the console command `modlockpress <mod> <node>` in the player's
  game. `pressCommand` in `panorama/src/draw.ts` builds it, so a host with its
  own renderer presses the same way. The server presses the node for the
  player who sent it, and only when that player's interface shows it as a
  button.

### Services

`callService(service, method, payload)` calls a service the host provides, and
`serve(service, handler)` answers the host's calls to one the mod serves. See
[How mods reach the game](#how-mods-reach-the-game).

### Spots

A spot is a base map and the solid objects players placed on it; nothing is
compiled. [`examples/dropper`](examples/dropper) builds one on Midtown: `/build`
opens the catalog for any player, and every player drops into the same spot.
The document in [`proto/modlock/spot/spot.proto`](proto/modlock/spot/spot.proto)
stores each model path and builder once, so a few hundred objects fit in a few
kilobytes.

## The manifest

Every project has a `mod.json` at its root:

```json
{
  "slug": "my-mod",
  "name": "My Mod",
  "version": "0.1.0",
  "language": "LANGUAGE_TYPESCRIPT"
}
```

| Field       | Required | Value                                                                               |
| ----------- | -------- | ----------------------------------------------------------------------------------- |
| `slug`      | yes      | The mod's identifier: lowercase letters, digits and hyphens.                        |
| `name`      | yes      | The name players see.                                                               |
| `version`   | yes      | A semantic version. Raise it for each publication.                                  |
| `language`  | yes      | `LANGUAGE_TYPESCRIPT`, `LANGUAGE_JAVASCRIPT`, `LANGUAGE_LUAU` or `LANGUAGE_PYTHON`. |
| `map`       |          | The [map](#map) the mod plays on.                                                   |
| `movement`  |          | A [movement model](#movement) that replaces how heroes move.                        |
| `abilities` |          | [Ability changes](#abilities) for the server and each player's game.                |
| `settings`  |          | [Choices](#settings) each player makes about the mod.                               |
| `metrics`   |          | [Totals](#metrics) the mod keeps for each player.                                   |

### Map

```json
"map": "hl_parry_ball"
```

`modlock dev` and `modlock play` start on the first map a loaded mod asks for,
or on the default map. The map must be installed in the game's `citadel/maps`
directory.

### Movement

```json
"movement": { "model": "MODEL_QUAKEWORLD", "scale": 1.25, "unpredictedButtons": "8753143351297" }
```

| Field                | Value                                                                                                  |
| -------------------- | ------------------------------------------------------------------------------------------------------ |
| `model`              | `MODEL_QUAKEWORLD`: heroes move by QuakeWorld's rules against the Quake collision in `maps/<map>.bsp`. |
| `scale`              | Game units per Quake unit.                                                                             |
| `unpredictedButtons` | A bit mask of the buttons the mod remaps or blocks on the server.                                      |

The player's game predicts the same movement. It leaves out the unpredicted
buttons, so it predicts no cast or shot the server will not make. Each landing
calls the mod's `onLanded` handler.

### Abilities

```json
"abilities": [
  {
    "ability": "gunslinger_rocket_launcher",
    "properties": { "AbilityCooldown": 0.8, "Damage": 120 },
    "fields": { "CitadelAbilityVData.m_projectileInfo/ProjectileInfo_t.m_flSpeed": 1250 },
    "copyFields": {
      "CAbilityRocketLauncherVData.m_ExplosionParticle": "CitadelAbilityVData.m_skillshotMissParticle"
    }
  }
]
```

Each entry retunes one stock ability on the server and in each player's game.

| Field        | Value                                                                            |
| ------------ | -------------------------------------------------------------------------------- |
| `ability`    | The ability to change.                                                           |
| `properties` | Ability properties, such as `AbilityCooldown`, and their new values.             |
| `fields`     | Float schema fields by `Class.m_field/Class.m_field` path, and their new values. |
| `copyFields` | Copy the field at each value's path over the field at its key's path.            |

### Settings

```json
"settings": [
  {
    "key": "layout",
    "label": "HUD layout",
    "kind": "KIND_CHOICE",
    "choices": [
      { "value": "full", "label": "Full" },
      { "value": "compact", "label": "Compact" }
    ],
    "default": "compact"
  },
  { "key": "sounds", "label": "Sounds", "kind": "KIND_SWITCH" },
  { "key": "volume", "label": "Volume", "kind": "KIND_NUMBER", "min": 0, "max": 100, "step": 5 }
]
```

| Kind          | Extra fields         | Value                 | Starts at without `default` |
| ------------- | -------------------- | --------------------- | --------------------------- |
| `KIND_CHOICE` | `choices`            | One choice's `value`  | The first choice            |
| `KIND_SWITCH` |                      | `"true"` or `"false"` | Off                         |
| `KIND_NUMBER` | `min`, `max`, `step` | A number on the step  | `min`                       |

A `key` is up to 64 letters, digits and underscores, not starting with a
digit. Every value is text: `player.setting(key)` reads it,
`player.setSetting(key, value)` changes it, and `settingOn` and
`settingNumber` read a switch or a number with its type. `onSettingChanged`
hears of changes made elsewhere, such as on a profile page. Bots read the
defaults. `modlock build` rejects a setting players could not choose from.

### Metrics

```json
"metrics": [
  { "name": "hud.opened", "kind": "KIND_COUNT", "labels": ["full", "compact"] },
  { "name": "round.best", "kind": "KIND_MAX" }
]
```

| Kind         | Keeps                        |
| ------------ | ---------------------------- |
| `KIND_COUNT` | How many times it was added. |
| `KIND_SUM`   | The sum of the added values. |
| `KIND_MAX`   | The largest added value.     |

A `name` is letters, digits, underscores and dots. Optional `labels` split a
metric, such as one total per HUD layout; pass the label as the third argument
of `player.addMetric(name, value, label)`. The host keeps the totals in memory
and hands them on once, when the player leaves or the mod stops. Without a
host service that takes them, the mod logs them, so `modlock dev` shows a
session's totals as it ends. Bots keep no totals.

### The built manifest

The `mod.json` that `modlock build` writes to `build/` adds the `runtime` that
runs the mod and its `entry` file. `modlock-host --plugin build` loads it.

## Writing a mod in Go

A Go mod is a `main` package that imports
[`github.com/paralin/modlock/mod`](mod) and registers its handlers in `init`:

```go
package main

import "github.com/paralin/modlock/mod"

func init() {
	mod.Command("hello", func(p mod.Player, args string) {
		_ = p.Chat("Hello from Go!")
	})
}

func main() {}
```

`modlock build` runs `go vet` and compiles the package for `wasip1` into
`build/mod.wasm`. Each call into the game returns an error, which a mod may
ignore to keep playing. Calls fail outside the game, so code that makes none,
such as the race rules in the [`race`](examples/race) example, tests with
plain `go test` on your machine. Go mods do not yet build interfaces.

[`mod/entity`](mod/entity) has the typed entity classes. Each field is a
method whose value gets and sets it, and each input a method that sends it. A
class has the methods its own class declares; convert it to a base class to
reach the base's, so
`entity.CBaseEntity(entity.CCitadelPlayerPawn{Handle: pawn.GetEntity()}).IHealth().Set(500)`
writes `m_iHealth`. Each designer name is a `Designer`, such as
`entity.NpcTrooperBoss`, whose `Create` takes its key value struct,
`entity.CNPC_TrooperBossKeys`.

## Writing a mod in Luau

A [Luau](https://luau.org) mod requires `@modlock` and registers its handlers
in `main.luau`:

```luau
local modlock = require("@modlock")

modlock.command("hello", function(player)
	player:chat("Hello from Luau!")
end)
```

`modlock build` installs the library into `.modlock/`, checks the types in
strict mode with `luau-analyze`, which it downloads the first time, and zips
the sources into `build/mod.zip`. `.luaurc` declares the library's alias, so
editors with the Luau language server resolve it too. A module requires
another by its relative path, such as `require("./round")`, as the
[`bounty`](examples/bounty) example does. The server runs the sources on
Luau compiled to WebAssembly, in the same sandbox and limits as every other
mod.

The library offers the calls of the TypeScript one under the same names, with
methods called as `player:chat(text)`. A 64-bit id, such as a Steam ID, is a
decimal string, because a Luau number holds integers exactly only up to
2^53. `modlock.has(bits, modlock.Buttons.attack)` tests a button or layer bit.
Luau mods do not yet build interfaces.

`@modlock/entities` has the typed entity classes; an entity reads a field by
indexing and writes one by assigning, and sends an input by calling the method
named after it. `entities.new` addresses an entity as a class, and
`entities.create` creates one of a designer name, as `create` does in
TypeScript. Both return `any`, so name the type where you keep the entity, and
the type of its spawn keys, for the analyzer to check them:

```luau
local entities = require("@modlock/entities")

local hero: entities.CCitadelPlayerPawn = entities.new("CCitadelPlayerPawn", pawn.entity)
hero.m_iHealth = (hero.m_iHealth or 0) + 50

local keys: entities.CNPC_TrooperBossKeys = { LaneNum = 2 }
local boss: entities.CNPC_TrooperBoss? = entities.create("npc_trooper_boss", { position = position, keys = keys })
```

A mod that requires the classes carries them in its build; one that does not
leaves them out.

## Writing a mod in Python

A Python mod imports `modlock` and registers its handlers with decorators in
`main.py`:

```python
import modlock


@modlock.command("hello")
def hello(player: modlock.Player, args: str) -> None:
    player.chat("Hello from Python!")
```

`modlock build` installs the library into `.modlock/`, checks the types in
strict mode with [Pyright](https://github.com/microsoft/pyright), which it
downloads with the Node.js that runs it the first time, and zips the sources
into `build/mod.zip`. `pyrightconfig.json` puts the library on the import
path, so editors with Pyright or Pylance resolve it too. A module imports
another by name, such as `import round`. The server runs the sources on
CPython 3.14 compiled to WebAssembly, in the same sandbox and limits as
every other mod. A mod imports the standard modules a game mod needs, such
as `dataclasses`, `enum`, `json`, `math`, `random` and `re`; modules that
reach files, processes or the network are not available.

The library offers the calls of the TypeScript one in snake_case, with
methods called as `player.chat(text)`. Messages are dataclasses, enums are
string literals such as `"match_intro"`, and integers, including 64-bit ids,
are Python integers. `modlock.Buttons` and `modlock.Layers` are flags, so
`pressed & modlock.Buttons.ATTACK` tests a button. Python mods do not yet
build interfaces or use typed entity classes.

## How mods reach the game

A mod is a WASI preview 1 reactor module. It imports two functions and exports
one:

| Name                                 | Direction   | Meaning                                                                                                                                                                                         |
| ------------------------------------ | ----------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `modlock.host_call(ptr, len) -> len` | mod to host | Hand the host an encoded `Call`; returns the length of the encoded `Reply`.                                                                                                                     |
| `modlock.host_read(ptr, len)`        | mod to host | Copy the host's pending message, a `Call` or a `Reply`, into mod memory.                                                                                                                        |
| `modlock_event(len) -> i64`          | host to mod | Deliver a `Call` of `len` bytes, which the mod copies with `host_read`. Returns the address and length of the encoded `Reply`, packed as `address << 32 \| length`, or zero for an empty reply. |

The calls are in [`proto/modlock/wasm.proto`](proto/modlock/wasm.proto). A
`Call` selects one method of the `Host` service, when a mod calls the game, or
of the `Mod` service, when the host delivers an event, and carries its encoded
request. The `Reply` carries the encoded response or an error. A new
capability is a new method; the functions never change. Each event runs
within a time budget, and each mod has a memory limit.

A program that embeds the host can offer mods more than the game: an
extension service, such as a game mode's match rules. The program provides a
service with `WasmHost::Provide`, and mods call it with the host's
`CallService` method. It calls a service a mod serves with `WasmHost::Call`,
which delivers the mod's `Serve` event. Each service defines its methods and
the encoding of their payloads. A mod cannot be called while it is calling the
host.

An interpreted mod's built `mod.json` sets its runtime, such as
`RUNTIME_QUICKJS`, in place of a module. The host runs the interpreter module
beside `modlock-host` and hands it the mod's source in the start event; mods
in one language share the compiled interpreter.

## The framework

The WebAssembly host is built on Modlock's C++ framework, which also serves
[native plugins](#native-plugins):

- **Plugin host.** `modlock-host` launches a listen server or a client, loads
  mods and plugins, and runs them through a fixed `Load`, `Start`, `Tick`,
  `Stop` lifecycle.
- **Engine events.** Subscribe to frames, chat, console commands, combat and
  damage, connections, respawns, and world start and end through
  `modlock::EngineHost`.
- **Entities and rendering.** Create and remove world text, particles, and
  effects; observe and control player pawns; spawn bots; trace rays.
- **Engine interop.** Signature scanning, relative call decoding, virtual table
  slot hooks, schema offsets, and `CEntityKeyValues` construction. Every
  byte signature lives in one table, and `modlock-sigcheck` checks the table
  against the game binaries after an update.
- **Session content.** Precache heroes and resources into the session manifest
  and advertise content addons to connecting clients.
- **Protocols.** Protobuf messages for mods, HUD text, announcements, chat,
  stamina, and camera paths, generated for C++, Go, TypeScript, Luau, and Python.
- **Portable tests.** Parsing, dispatch, sandbox, and fixture tests run on macOS
  and Linux without the game.

The game runtime is Windows x64, including Windows builds under Proton.

## Building

Requirements: Go (the version in `go.mod`), CMake 3.24 or newer, and a C++23
compiler. CMake downloads [wasi-sdk](https://github.com/WebAssembly/wasi-sdk)
to compile the QuickJS and Luau runtimes to WebAssembly.

```sh
git submodule update --init --recursive
go mod download github.com/aperturerobotics/protobuf github.com/aperturerobotics/abseil-cpp

jobs=$(( $(getconf _NPROCESSORS_ONLN) / 2 ))
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$jobs"
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/build/sdk"
```

Protobuf and abseil take a long time to build. Use half the cores, as shown, so
the build leaves the machine usable.

CMake downloads the [Wasmtime C API](https://docs.wasmtime.dev/c-api/) release
for the target platform. Pass `-DMODLOCK_WASMTIME_DIR=<extracted release>` to
build offline. Install puts the Wasmtime library next to `modlock-host`.

- **Windows:** run `scripts/win-build.ps1` from a Visual Studio developer shell.
- **macOS or Linux, targeting Windows:** `scripts/proton-build.sh` cross-builds
  the Windows SDK with Zig.

To build from a source archive without `.git`, pass its commit with
`-DMODLOCK_REVISION=<40-character SHA>`.

`modlock-host --dump DIRECTORY` starts the dedicated server, writes the game
as it describes itself to `schemas.json` (schema classes and enums),
`entities.json` (designer names, key values, inputs and outputs) and
`console.json` (console variables and commands), and quits. The documents
follow [`proto/modlock/dump/dump.proto`](proto/modlock/dump/dump.proto) and are
sorted by name, so two builds' dumps diff cleanly. After a game update, replace
[`data/dump`](data/dump) with a new dump and regenerate the entity classes:

```sh
scripts/gen-entities.sh
```

`bun install` installs a pre-commit hook. The hook formats the staged files
with oxfmt and restages them. It then checks the staged TypeScript for commented
code paragraphs with `scripts/tsstyle.ts` and lints it with type-aware oxlint.
`bun run typecheck`, `bun run lint`, `bun run format` and `bun run test` run the
same tools across the repository. `bun run lint:cc` reports the includes a C++
source does not use, and `bun run lint:cc --fix` removes them; it reads the
compile databases of `build` and `scripts/proton-build.sh`, so build both
first. Lint the example mods after
`go run ./cmd/modlock build examples/<mod>` installs their typings.

## Native plugins

Native C++ plugins extend the host itself: new engine hooks, new host calls for
mods, and framework features. They have full access to the game process and
none of the sandbox's protection. The installed SDK exports `modlock::sdk` through CMake:

```cmake
find_package(Modlock CONFIG REQUIRED)
add_library(my_plugin MODULE my_plugin.cc)
target_link_libraries(my_plugin PRIVATE modlock::sdk)
target_compile_features(my_plugin PRIVATE cxx_std_23)
```

A plugin implements `modlock::Plugin` and exports three functions. `Load`,
`Tick`, and `Stop` are optional overrides:

```cpp
#include "modlock/engine_host.h"
#include "modlock/plugin_library.h"

class MyPlugin final : public modlock::Plugin {
 public:
  explicit MyPlugin(const modlock::PluginContext& context) : engine_(context.engine) {}
  uint32_t InterfaceVersion() const override { return modlock::PluginInterfaceVersion; }
  const char* Name() const override { return "my-plugin"; }
  bool Start() override {
    auto command = engine_->OnCommand([](int32_t, std::string_view text) {
      return text == "hello";  // true claims the command
    });
    if (!command) return false;
    command_ = std::move(*command);
    return true;
  }
  void Stop() override { command_.Reset(); }

 private:
  modlock::EngineHost* engine_;
  modlock::Subscription command_;
};

MODLOCK_PLUGIN_EXPORT modlock::PluginManifest ModlockPluginManifest_v1() {
  return {modlock::PluginInterfaceVersion, MODLOCK_SDK_ABI};
}
MODLOCK_PLUGIN_EXPORT modlock::Plugin* ModlockPluginCreate_v1(const modlock::PluginContext* c) {
  return c ? new MyPlugin(*c) : nullptr;
}
MODLOCK_PLUGIN_EXPORT void ModlockPluginDestroy_v1(modlock::Plugin* p) { delete p; }
```

[`examples/hello`](examples/hello) is a complete plugin that creates a world
text entity, runs a console command, removes the entity before world shutdown,
and exits. Build and run it against the installed SDK:

```sh
cmake -S examples/hello -B build-hello -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/build/sdk"
cmake --build build-hello --parallel "$jobs"

modlock-host --check-plugin --plugin <hello library>
modlock-host --game-dir <Deadlock installation> --plugin <hello library>
```

| Option               | Effect                                                                                                                                           |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| `--plugin PATH`      | Load a built mod (a directory with `mod.json`, or a `.wasm` file) or a plugin library; repeat for several.                                       |
| `--control ADDRESS`  | Report mod starts, logs, failures and player joins to the controller at `ADDRESS`, and take reloads from it. The `modlock` command line uses it. |
| `--settings PATH`    | Keep players' mod settings in the JSON file at `PATH`; without it they last for the run. A native plugin may keep them instead.                  |
| `--check-plugin`     | Load, start, and stop the plugins without opening game modules.                                                                                  |
| `--game-dir DIR`     | Run a listen server from the Deadlock installation at `DIR` (or `DEADLOCK_DIR`).                                                                 |
| `--map NAME`         | Start on `NAME` (default `dl_midtown`).                                                                                                          |
| `--hostport PORT`    | Serve on UDP `PORT` (default 27067).                                                                                                             |
| `--connect ADDRESS`  | Run a client that joins `ADDRESS`, with the plugin loaded in the client.                                                                         |
| `--engine-args ARGS` | Append engine command-line arguments in either role.                                                                                             |
| `-- ARGS`            | Pass the remaining arguments to the plugin.                                                                                                      |

Clients run with `-insecure` and cannot join VAC-secured servers. See
`modlock-host --help` for every option.

## Plugin contract

**ABI.** Before it creates a plugin, the loader checks that the plugin was built
with the same interface version, SDK source, public headers, compiler, standard
library, build mode, and runtime configuration as the host. A plugin loads only
into the host it was built with, so build and ship the host, SDK, and plugins
together.

**Engine access.** The host loads the engine modules and installs the native
hooks. Plugins call the engine through `PluginContext::engine` and keep the
`Subscription`s it returns. Resetting a subscription removes the callback and
frees what it captured.

**Sandboxed mods.** `PluginContext::wasm` lends the host's `WasmHost`. A
plugin that hosts games of its own, such as game modes published as sandboxed
mods, loads them there, provides them services with `Provide` and calls theirs
with `Call`. The plugin drives each mod's `Start`, `Tick` and `Stop` itself.

**Dispatch.** Callbacks run on the engine thread in the order they were
registered. For a command, damage, or respawn decision, the first callback that
returns true handles it, and later callbacks are not called.

**Lifecycle.** `Load`, `Start`, and `Stop` each run once, in order, with `Tick`
once per frame between `Start` and `Stop`. If startup fails, every plugin that
began starting stops in reverse order, including the one that failed. Remove
live entities in `OnWorldEnding`, before the engine tears down the world, and
drop views of the previous world in `OnWorld`. `Stop` runs after the engine
returns. Each plugin is destroyed inside its own library before that library
unloads. Plugins cannot be reloaded while the host runs.

[Engine access](docs/engine-access.md) covers module lookup, signature
resolution, platform boundaries, and hook lifetime.

## Protocols

Messages live in [`proto/modlock`](proto/modlock) and keep the `modlock` wire
package. `bun run gen` regenerates the Go, TypeScript, Luau, Python, and C++ code with the
pinned protobuf toolchain.

The repository commits no built JavaScript. `modlock build` builds the
TypeScript library in [`js/`](js) from the Modlock source when the source is
on disk, in a checkout or the Go module cache, and caches it. A release build
downloads the library its release publishes instead.

## Acknowledgments

Thank you to the Deadlock modding community, whose shared research made this
project possible, and especially to [Deadworks], whose signatures, engine
calling conventions, and startup sequence Modlock learned from. See
[ATTRIBUTION.md](ATTRIBUTION.md) for details.

[Deadworks]: https://github.com/Deadworks-net/deadworks

## License

MIT. See [LICENSE](LICENSE).
