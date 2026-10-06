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

> **Early development.** APIs change without notice. TypeScript, JavaScript,
> Luau and Python mods, the `modlock` command line and `modlock publish` work
> today.

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
[releases](https://github.com/paralin/modlock/releases). It runs the Windows
server on Windows, or through Steam's Proton on Linux, and fetches the server
that matches its release the first time. Mods need nothing else installed.

```sh
modlock new my-mod
cd my-mod
modlock dev
```

`modlock new` writes a TypeScript project: `mod.json` names the mod, `main.ts`
answers `/hello`, and `tsconfig.json` lets your editor check the code as you
type. `--language` picks `javascript`, `luau` or `python` instead.

`modlock dev` builds the mod, starts a local server with it, and launches
Deadlock through Steam to join. Each time you save, it checks and rebuilds the
mod and swaps it into the running server; a build that fails keeps the
previous one running and prints the errors. The mod's log lines, reloads and
player joins appear in the terminal, and the server console goes to
`build/server.log`.

Without the game, as on macOS or a computer without Deadlock, `modlock dev`
runs the mod in the sandbox instead: the same WebAssembly runtime and limits,
with a stand-in player who has no hero. Type a command such as `/hello` and
press Enter to send it as the player. The mod's log lines, the chat and
announcements it sends the player, and its reloads appear in the terminal;
calls that need the game, such as a hero's position, answer empty. `--sandbox` runs there even
where the game runs. With `--json`, a program such as an editor sends each
line of input as a `modlock.cli.Input`, a command or a button press, and
draws the interface from the events.

| Command                 | Effect                                                                                                       |
| ----------------------- | ------------------------------------------------------------------------------------------------------------ |
| `modlock new DIR`       | Create a mod project named after `DIR`, in TypeScript unless `--language` names another.                     |
| `modlock build`         | Check the mod and write the built mod to `build/`.                                                           |
| `modlock dev`           | Run the mod in a local server, join it, and reload it on each save; without the game, run it in the sandbox. |
| `modlock play [MOD...]` | Run built mods in a local server and join it, or in the sandbox without the game.                            |
| `modlock publish`       | Build and check the mod, then publish it.                                                                    |

`--no-game` runs only the server, `--port` changes its UDP port, and each
`--arg VALUE` passes an argument to the mods' start handlers. Set
`DEADLOCK_DIR` when Deadlock is outside the Steam libraries, `MODLOCK_HOST` to
use a `modlock-host.exe` you built (the sandbox reads the interpreters beside
it), and `MODLOCK_PROTON` to choose a Proton installation.

## Making a game mode

The project `modlock new` writes answers one command. A game mode grows from
there in the same few steps each time. [`examples/hello-ts`](examples/hello-ts)
is a small mod you can run that shows most of them: commands, a JSX menu
with buttons, floating text it moves each frame, a setting and a metric.
[The TypeScript library](#the-typescript-library) lists every call.

### Wire the events, keep the state in one place

A mod is a set of handlers the server calls. Keep `main.ts` a short list of
that wiring, and keep the game's state in one object the handlers call into:

```ts
import { command, onDamage, onFrame, onStart, onWorld } from 'modlock'

import { Match } from './match'

const match = new Match()

onStart((args) => match.start(args))
onWorld(() => match.world())
onFrame((frame) => match.frame(frame))
onDamage((hit) => match.damage(hit))
command('ready', (player) => match.ready(player))
```

Split a large mode into modules by concern, such as the match rules, the arena
it draws and the HUD it shows. `modlock build` bundles every module `main.ts`
imports and checks their types together.

### Run the match on the frame

The server calls `onFrame` once per frame. Hold the match in a phase, such as
waiting, countdown, live and between rounds, and move between phases by the
game clock in `frame.timeSeconds`. Store when each phase ends and compare it
with the clock; the frame rate varies, so do not count frames.

```ts
frame(frame: FrameEvent): void {
  this.now = frame.timeSeconds
  if (this.phase === 'countdown' && this.now >= this.liveAt) {
    this.phase = 'live'
    for (const player of this.players) {
      player.freeze(false)
    }
  }
}
```

Each event runs within a time budget, and a mod that overruns it stops for the
rest of the match. Keep each frame's work small.

### Find the players and their heroes

`players()` lists who is connected now, bots included; call it when you need
it rather than keeping your own list. A connection's `generation` changes when
someone new takes the slot, so key per-player state by slot and generation.
`player.pawn()` reads the hero this frame: it is `undefined` until the hero
spawns, and a hero with no health left is dead. `selectHero`, `teleport`,
`freeze` and `respawn` move heroes through the match. Call `precache` from
`onStart` with the heroes and models the mode uses, so they load with the
world.

### Start over with each world

`onWorld` runs each time a map loads, and at start when one already has.
Objects and bots from the earlier world are gone by then, so forget them in
`onWorld` and place the new world's objects there.

### Decide the fights

`onDamage` sees each hit before it lands. Return `{ block: true }` to stop it,
for spawn protection or between rounds, or `{ amount }` to change it.
`onDamaged` reports each hit that landed, for scores and effects that follow
a hit.

### Tell players what is happening

| Call                                          | Use it for                                          |
| --------------------------------------------- | --------------------------------------------------- |
| `toast(player, text)`                         | Answering a command, or saying why it was refused.  |
| `player.announce(title, text)`                | Round starts and results, announced to each player. |
| `show(player, <panel>…</panel>)`              | A HUD that stays on screen, written in JSX.         |
| `createText`, `createModel`, `createParticle` | Signs, props and effects in the world.              |

`show` sends only what changed, so redraw a player's HUD from the match state
on every frame instead of tracking what to update.

### Let players choose

Commands are the simplest controls: `command('vote', (player, args) => …)`
receives the text after `/vote`. Buttons in a HUD call back into the mod the
same way. A choice a player keeps between matches, such as a HUD layout,
belongs in a setting: [the manifest](#the-manifest) declares it, and
`player.setting(key)` reads the player's value.

### Test it

The sandbox runs commands and interfaces in seconds; use it while you write
the rules. For a whole round, let the mod test itself: started with an
argument such as `modlock play --arg probe`, it seats bots with `addBot`,
plays the round, checks the outcome, logs PASS or FAIL and quits the server
with `serverCommand('quit')`. [`examples/dropper`](examples/dropper) does this.

When the mode plays well, [publish it](#publishing).

## Publishing

`modlock publish` builds the mod and runs the check every host runs before it
loads a mod: the module may import only the Modlock and WASI functions, and it
must start and answer its first event within the time and memory limits. A mod
that passes then goes where `--to` names:

| `--to`      | Effect                                                                                                                                                                                                                             |
| ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `hyperline` | Upload the mod to [hyperline.gg](https://hyperline.gg), the default. The first publish opens the browser to sign in; the session is saved in the user configuration directory. `--origin` names another service with the same API. |
| `archive`   | Write `SLUG-VERSION.zip` to `--out`: the mod, `modlock.exe` and `Play.cmd`, which players unpack and run on Windows.                                                                                                               |
| `github`    | Attach that zip to the GitHub release `SLUG-vVERSION` of the project's repository, creating the release if needed. It uses the [`gh`](https://cli.github.com) command line and its sign-in.                                        |

A release carries the built `mod.json` and its entry. Hyperline publishes
each version once, so raise `version` in `mod.json` before publishing again.

## The TypeScript library

A TypeScript or JavaScript mod imports `modlock` and registers its handlers
when it loads. `main.ts`, or `main.js`, is the entry.

`modlock build` installs the `modlock` library into `node_modules/`, checks
the types with the TypeScript native compiler, which it downloads the first
time, and bundles the mod into `build/mod.js`. A JavaScript mod is checked
from its JSDoc. No Node.js or npm is needed. The server runs the bundle on
[QuickJS](https://github.com/quickjs-ng/quickjs), itself compiled to
WebAssembly, so a script mod has the same sandbox and limits as a compiled
one.

| Call                                                                                                   | Effect                                                                                                                        |
| ------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------- |
| `command(name, handler)`                                                                               | Run `handler` when a player types `/name` in chat.                                                                            |
| `onFrame(handler)`                                                                                     | Run `handler` once per server frame.                                                                                          |
| `onStart(handler)`                                                                                     | Run `handler` when the server starts the mod, with the arguments after `--`.                                                  |
| `onWorld(handler)`                                                                                     | Run `handler` with the map's name each time a world has loaded.                                                               |
| `log(...)` and `console.log(...)`                                                                      | Write a line to the server log under the mod's name.                                                                          |
| `serverCommand(line)`                                                                                  | Run a line at the server console.                                                                                             |
| `players()`                                                                                            | List the connected players and bots.                                                                                          |
| `player.chat(text)`, `player.centerText(text)`, `player.announce(title, text)`                         | Show text to one player.                                                                                                      |
| `player.pawn()`                                                                                        | Read the player's hero: health, team, position, aim and stamina.                                                              |
| `player.selectHero`, `respawn`, `clearItems`, `freeze`, `restoreStamina`, `refreshAbility`, `teleport` | Control the player's hero.                                                                                                    |
| `blockInput(buttons)`, `player.blockInput(buttons)`, `player.press(buttons)`                           | Withhold buttons from every hero or one, or press them for one.                                                               |
| `remapInput(from, to, repeat)`                                                                         | Make one button act as another for every hero.                                                                                |
| `createModel`, `createText`                                                                            | Place a model or floating text; move, retext or remove it later.                                                              |
| `loadSpot(spot)`, `encodeSpot`, `decodeSpot`                                                           | Place a spot's solid objects on the running map, launch heroes from its bounce pads, and save it as a compact document.       |
| `new Dropper(catalog, spot)`                                                                           | Let players build a spot while they play: pick an object, aim a preview, drop, move, delete and undo.                         |
| `addBot`, `removeBot`                                                                                  | Add or remove a bot player.                                                                                                   |
| `precache({heroes, resources})`                                                                        | Load heroes and resources with the next world.                                                                                |
| `player.abilities()`, `setAbility`                                                                     | Read the hero's abilities, or set one's upgrades and charges.                                                                 |
| `player.giveItem`, `replaceAbility`, `holdModifier`                                                    | Give the hero an item, swap an ability slot, or keep an ability's modifier on the hero.                                       |
| `modifierState(entity, state)`, `holdModifierState(entity, state, active)`                             | Report whether an entity has a modifier state, or hold one on it.                                                             |
| `readField(entity, class, field, type)`                                                                | Read any schema field of a live entity by name.                                                                               |
| `moveEntity`, `emitSound`                                                                              | Move an entity and set its velocity, or play a sound on it.                                                                   |
| `player.kill()`, `setVelocity`, `buttons()`                                                            | Kill the hero, set its velocity, or read the buttons it holds.                                                                |
| `player.watchMovement(true)`                                                                           | Add the hero's movement state and the game's movement facts, such as landings and wall jumps, to each new tick's frame event. |
| `watchProjectiles(options)`, `onLaunch`, `onImpact`                                                    | Watch projectiles by name: see each one's first frame and decide its impact.                                                  |
| `onLanded(handler)`                                                                                    | Run `handler` when a hero lands under the manifest's movement model.                                                          |
| `onDamage(handler)`, `onDamaged(handler)`                                                              | Block or change each hit before it lands, or hear of each hit after.                                                          |
| `toast(player, text)`                                                                                  | Show a player a short notice that fades.                                                                                      |
| `show(player, element)`, `hide(player)`                                                                | Show a player an interface written in JSX, or remove it.                                                                      |
| `callService(service, method, payload)`, `serve(service, handler)`                                     | Call a service the host provides, or answer the host's calls to one the mod serves.                                           |

`modlock/entities` has a typed class for each server entity class, such as
`new CCitadelPlayerPawn(pawn.entity).m_iHealth`; a mod's bundle keeps only
the classes it uses.

The calls above, and every other one, are generated from the `Host` service in
[`proto/modlock/wasm.proto`](proto/modlock/wasm.proto), which documents each.
A call that fails logs the failure and returns `false` or `undefined`; it never
stops the mod. Everything a mod places leaves with the world, and stopping or
reloading the mod also removes it and releases frozen heroes, held modifier
states and input.

A spot is a base map and the solid objects players placed on it; nothing is
compiled. [`examples/dropper`](examples/dropper) builds one on Midtown:
`/build` opens the catalog for any player, and every player drops into the
same spot. The document in
[`proto/modlock/spot/spot.proto`](proto/modlock/spot/spot.proto) stores each
model path and builder once, so a few hundred objects fit in a few kilobytes.

An interface is JSX in a `.tsx` file, built from four elements: `panel`,
`label`, `image` and `button`, styled with a closed set of layout and paint
properties:

```tsx
show(
  player,
  <panel style={{ flow: 'down', horizontalAlign: 'center', margin: [80, 0] }}>
    <label style={{ fontSize: 32, bold: true }}>Round {round}</label>
    <button onPress={(player) => ready(player)}>Ready</button>
  </panel>,
)
```

`show` sends only what changed since the last call, so a mod may call it on
every frame. The interface travels as data, the typed tree in
[`proto/modlock/ui.proto`](proto/modlock/ui.proto): the player's game draws it
with stock panels and runs no mod code, and a button press comes back to its
`onPress`. The host clears a player's interface when the player leaves or the
mod stops.

The player's game draws interfaces with the renderer in
[`panorama/`](panorama): the layout `panorama/layout/modlock/ui.xml` and its
script, which each release publishes in `modlock-library.tar.gz` as `ui.js`
for `scripts/modlock/ui.js`; `go run ./cmd/modlock-library <archive>` builds
it from a checkout. A client content package loads the
layout into a panel that stays loaded through matches; the renderer draws over
the HUD. `modlock dev` and `modlock play` serve the local player's interfaces
on `127.0.0.1:4320` (`--ui-port`, 0 for none). Deadlock's HTML panels load only
https pages, so the renderer reads them through a relay page on an https origin
that `--ui-relay` names, `https://hyperline.gg` by default; the controller
answers only that origin's page in the game's own browser.

A press runs the console command `modlockpress <mod> <node>` in the player's
game, which `pressCommand` in `panorama/src/draw.ts` builds, so a host that
draws interfaces with its own script presses the same way. The server presses
the node for the player who sent it, and only when that player's interface
shows it as a button.

## The manifest

A project is a directory with `mod.json`, which names the mod and its source
language:

```json
{
  "slug": "my-mod",
  "name": "My Mod",
  "version": "0.1.0",
  "language": "LANGUAGE_TYPESCRIPT"
}
```

An optional `"map"` names the map the mod plays on, such as `"hl_parry_ball"`;
`modlock dev` and `modlock play` start the first map a loaded mod names, and
the default map otherwise. The map must be installed in the game's
`citadel/maps` directory.

An optional `"movement"` replaces the heroes' movement. With
`{"model": "MODEL_QUAKEWORLD", "scale": 1.25}` every hero moves by
QuakeWorld's rules against the Quake collision in `maps/<map>.bsp`, scaled by
game units per Quake unit, and each landing calls the mod's landing handler.
The player's game predicts the same movement; `"unpredictedButtons"` names
buttons the mod remaps or blocks on the server, which the prediction leaves
out.

An optional `"abilities"` list retunes stock abilities in both the server and
each player's game: each entry names an ability, then sets its `properties`,
such as `AbilityCooldown`, its float schema `fields` by
`Class.m_field/Class.m_field` path, and its `copyFields`.

An optional `"settings"` list declares choices each player makes about the
mod, such as a HUD layout. Each entry has a `key`, a `label` and a `kind`:
`KIND_CHOICE` with `choices` of `{"value", "label"}`, `KIND_SWITCH`, or
`KIND_NUMBER` with `min`, `max` and `step`. An optional `default` is the value
a player starts with; without one a player starts at the first choice, off,
or `min`. The mod reads a player's value with the player's `setting` method,
changes it with `setSetting`, and reads a switch or a number with types through
`settingOn` and `settingNumber`. It hears of changes made elsewhere, such as on
a profile page, through its setting-changed handler. Bots read the defaults.
`modlock build` refuses a declaration players could not choose from.

An optional `"metrics"` list declares measures the mod keeps for each player,
such as how often a HUD panel opens. Each entry has a `name` of letters, digits,
underscores and dots, a `kind` (`KIND_COUNT` counts calls, `KIND_SUM` adds
values, `KIND_MAX` keeps the largest) and optional `labels`, such as one per
HUD layout. The mod adds to a player's total with the player's `addMetric`
method, naming one of the labels when the metric has them. The host keeps the
totals in memory and hands them on once, when the player leaves or the mod
stops; without a host service that takes them, the mod logs them, so
`modlock dev` shows a session's totals as it ends. Bots keep no totals.

The built mod in `build/` has its own `mod.json` naming the runtime and the
entry; `modlock-host --plugin build` loads it.

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
the sources into `build/mod.zip`. `.luaurc` names the library's alias, so
editors with the Luau language server resolve it too. A module requires
another by its relative path, such as `require("./round")`. The server runs
the sources on Luau compiled to WebAssembly, in the same sandbox and limits
as every other mod.

The library offers the calls of the TypeScript one under the same names, with
methods called as `player:chat(text)`. A 64-bit id, such as a Steam ID, is a
decimal string, because a Luau number holds integers exactly only up to
2^53. `modlock.has(bits, modlock.Buttons.attack)` tests a button or layer bit.
Luau mods do not yet build interfaces or use typed entity classes.

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
`Call` names one method of the `Host` service, when a mod calls the game, or
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

An interpreted mod's built `mod.json` names its runtime, such as
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
compiler. The tests build the Go example mod, so they also need Go.

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

After a game update renames entity fields, regenerate the entity classes from
the schema that [DumpSource2](https://github.com/ValveResourceFormat/DumpSource2)
writes, such as the `DumpSource2/schemas` directory of
[GameTracking-Deadlock](https://github.com/SteamDatabase/GameTracking-Deadlock):

```sh
scripts/gen-entities.sh path/to/DumpSource2/schemas
```

`bun install` installs a pre-commit hook. The hook formats the staged files
with oxfmt and restages them. It then checks the staged TypeScript for commented
code paragraphs with `scripts/tsstyle.ts` and lints it with type-aware oxlint.
`bun run typecheck`, `bun run lint`, `bun run format` and `bun run test` run the
same tools across the repository. Lint the example mods after
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
