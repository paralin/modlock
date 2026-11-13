<div align="center">

<h1>⚙️ Modlock</h1>

<p><b>Custom game modes for Deadlock, in a language you already know.</b></p>

<a href="https://discord.gg/DCErWrCFvh"><img alt="Join the Discord" src="https://img.shields.io/badge/Join_the_Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white"></a>
<a href="#getting-started"><img alt="Install" src="https://img.shields.io/badge/Install-2b2b2b?style=for-the-badge&logo=github&logoColor=white"></a>
<a href="https://hyperline.gg"><img alt="Publish on hyperline.gg" src="https://img.shields.io/badge/Publish_on-hyperline.gg-e4572e?style=for-the-badge"></a>

<p>
<img alt="TypeScript" src="https://img.shields.io/badge/TypeScript-3178C6?style=flat-square&logo=typescript&logoColor=white">
<img alt="JavaScript" src="https://img.shields.io/badge/JavaScript-F7DF1E?style=flat-square&logo=javascript&logoColor=black">
<img alt="Luau" src="https://img.shields.io/badge/Luau-00A2FF?style=flat-square&logo=lua&logoColor=white">
<img alt="Python" src="https://img.shields.io/badge/Python-3776AB?style=flat-square&logo=python&logoColor=white">
<img alt="Go" src="https://img.shields.io/badge/Go-00ADD8?style=flat-square&logo=go&logoColor=white">
<img alt="License: MIT" src="https://img.shields.io/badge/License-MIT-green?style=flat-square">
</p>

</div>

**Modlock** lets you write the logic of a [Deadlock] custom game mode by hand,
in TypeScript, JavaScript, Luau, Python or Go, and the game server runs your
mod in a WebAssembly sandbox. Every language's library makes its calls to the
game as messages from one protobuf schema, so each language gets the same
events and calls. `modlock publish` uploads mods to [hyperline.gg], the default
platform for custom games.

[Deadlock]: https://store.steampowered.com/app/1422450/Deadlock/
[hyperline.gg]: https://hyperline.gg

```ts
import { command } from 'modlock'

command('hello', (player) => {
  player.chat('Hello from TypeScript!')
})
```

> **Early development.** APIs may change without notice between any two
> releases. [TypeScript, JavaScript](https://paralin.github.io/modlock/languages/typescript),
> [Luau](https://paralin.github.io/modlock/languages/luau), [Python](https://paralin.github.io/modlock/languages/python) and
> [Go](https://paralin.github.io/modlock/languages/go) mods, the `modlock` command line and
> `modlock publish` all work today.

## Why WebAssembly

- **Safe to share.** A mod runs in a [Wasmtime] sandbox inside the server,
  where it can reach only the calls the schema offers.
  Those calls let it read and write its own files, but it has no network
  access and no environment variables. When a mod
  crashes, loops forever or allocates past its limit, the server stops that
  mod, logs why and keeps the match running. A slow event still finishes, and
  the players see a warning in chat. Any player can restart a stopped or slow
  mod with `/restartmod`.
- **Any language.** You can write a mod in any language that compiles to
  WebAssembly, using a small library for that language.
- **Built once.** The server looks up each field and input a mod uses by name,
  so a mod built before a game update still works after it.
- **Fast.** Wasmtime compiles mods to machine code with Cranelift. The server
  calls a mod inside the game frame and waits for its answer, so the mod can
  claim a command or change that frame before the game moves on.

[Wasmtime]: https://wasmtime.dev

## Getting started

The only program you need is the `modlock` command. Install it by running the
line for your computer.

Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/paralin/modlock/master/install.ps1 | iex
```

macOS or Linux, in a terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/paralin/modlock/master/install.sh | sh
```

Or install it with npm on any of them, into a project or globally:

```sh
npm install --save-dev @modlock/modlock
npx modlock new my-mod
```

You need nothing else for a TypeScript, JavaScript, Luau or Python mod, and a
Go mod also needs [Go](https://go.dev/dl/). The
[install page](https://paralin.github.io/modlock/start/install) explains how to
download `modlock` by hand and what to do when your terminal cannot find it.

Then make a mod and run it:

```sh
modlock new
cd my-mod
modlock dev
```

`modlock new` asks for the mod's name and its language. `modlock dev` builds the
mod, starts a local server and launches Deadlock through Steam to join it. Each
time you save, it rebuilds the mod and replaces the running one. The
starter mod replies to `/hello` in chat, and
[Your first mod](https://paralin.github.io/modlock/start/first-mod) walks
through the rest.

Complete game modes to start from include [`arena`](examples/arena) in
TypeScript, [`bounty`](examples/bounty) in Luau and [`race`](examples/race) in
Go, all in the [`examples`](examples) directory.

## Documentation

The documentation is at [paralin.github.io/modlock](https://paralin.github.io/modlock/).
Every sample there comes in TypeScript, Luau, Python and Go, and once you
choose your language, every page shows its samples in it.

| Section                                                             | Holds                                                                     |
| ------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| [Start](https://paralin.github.io/modlock/start/install)            | Install, your first mod, a tutorial and the examples.                     |
| [Languages](https://paralin.github.io/modlock/languages/typescript) | What is particular to TypeScript, Luau, Python and Go.                    |
| [Guides](https://paralin.github.io/modlock/guides/events)           | One page for each thing a mod does: events, players, timers, the world.   |
| [Mechanics](https://paralin.github.io/modlock/mechanics/ready-up)   | Whole mechanics to start from: a countdown, kill scoring, a hill, a race. |
| [Reference](https://paralin.github.io/modlock/reference/cli)        | The command line, the manifest and publishing.                            |
| [Internals](https://paralin.github.io/modlock/internals/protocol)   | The protocol, the framework, building from source and native plugins.     |

The site's source is in [`docs`](docs), and running `bun install` and
`bun run dev` there serves it locally. Complete mods to read and run are in [`examples`](examples).

## Acknowledgments

Thanks to the Deadlock modding community, whose research made this possible, and
to [Deadworks], whose signatures, calling conventions and startup sequence
taught Modlock. See [ATTRIBUTION.md](ATTRIBUTION.md).

[Deadworks]: https://github.com/Deadworks-net/deadworks

## License

MIT. See [LICENSE](LICENSE).

---

<div align="center">

<b>Questions, ideas or a mod to show off?</b>

<a href="https://discord.gg/DCErWrCFvh"><img alt="Join the Discord" src="https://img.shields.io/badge/Join_the_Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white"></a>

</div>
