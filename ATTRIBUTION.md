# Attribution

Modlock builds on work shared by the Deadlock modding community. This file
records what came from other projects and under which license.

## Deadworks

[Deadworks](https://github.com/Deadworks-net/deadworks) is a server-side modding
framework for Deadlock, released under the MIT License; its copyright line
appears in [LICENSE](LICENSE). Modlock used it as a reference implementation
for these parts of the engine interface:

- **Signatures.** The byte patterns in `data/game_signatures.txtpb`
  started from the Deadworks signature database (`deadworks_mem.jsonc`) and
  were then checked against the live game binaries.
- **Virtual table slots.** The `GameFrame` slot hooked in
  `src/gameinterop/frame_hook.cc`, the `ClientPutInServer` slot in
  `src/gameinterop/connection_tracker.cc`, and the `CBaseEntity::GetMaxHealth`
  slot in `src/gameinterop/pawn_observer.cc` match the slots Deadworks uses.
- **Entity creation.** The `CreateEntityByName`, `CEntityKeyValues` and
  `QueueSpawnEntity` calling sequence in `src/gameinterop/keyvalues.cc` and
  `src/render/world_text_game_factory.cc` follows Deadworks'
  `EntitySystemHelper` and its `point_worldtext` creation.
- **Server startup.** The listen-server handoff in `src/net/listen_boot.cc`
  follows the startup order in Deadworks' `deadworks/src/startup.cpp`.
- **Session manifest.** The manifest builder and hero precache offsets in
  `src/gameinterop/game_rules.cc` follow Deadworks' hero precache hook.
- **Plugin lifecycle.** The plugin host in `src/host.cc` follows Deadworks'
  plugin lifecycle, without its hot reload.
- **Vendored hooking library.** `third_party/safetyhook` is the amalgamated
  distribution vendored in Deadworks' `vendor/` directory, copied unchanged.

## Deadlock Dolly

The camera path evaluation in `src/camera/camera_path.cc` and
`include/modlock/camera/camera_path.h` is ported from
[Deadlock Dolly](https://github.com/cravvnn/deadlock-dolly) (`dolly/path.py`
and `native/include/dolly_flight.hpp`), MIT License, Copyright (c) 2026
Deadlock Dolly contributors. The notice is kept in those files.

## Third-party libraries

| Library | Location | License |
| --- | --- | --- |
| [safetyhook](https://github.com/cursey/safetyhook) | `third_party/safetyhook/safetyhook.*` | Boost Software License 1.0, in `LICENSE` |
| [Zydis](https://github.com/zyantific/zydis) | `third_party/safetyhook/Zydis.*` | MIT, in `LICENSE.Zydis` |
| [GoogleTest](https://github.com/google/googletest) | `third_party/googletest` (tests only) | BSD 3-Clause |
| [QuickJS-ng](https://github.com/quickjs-ng/quickjs) | Compiled into `quickjs.wasm` | MIT, installed as `quickjs-LICENSE` |
| [Luau](https://github.com/luau-lang/luau) | Compiled into `luau.wasm`; `luau-analyze` checks Luau mods | MIT, installed as `luau-LICENSE` |
| [CPython](https://github.com/python/cpython) | Compiled into `python.wasm` with its standard library, from the [CPython WASI builds](https://github.com/brettcannon/cpython-wasi-build) | Python Software Foundation License, installed as `python-LICENSE` |
| [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) | Its C library and unwinder are linked into `luau.wasm` and `python.wasm` | Apache License 2.0 with LLVM exceptions, and MIT |
| [Wasmtime](https://github.com/bytecodealliance/wasmtime) | Linked into `modlock-host`; its `wizer` command snapshots `python.wasm` at build time | Apache License 2.0 with LLVM exceptions |
| [protobuf-es-lite](https://github.com/aperturerobotics/protobuf-es-lite) | Bundled into the TypeScript library, the interface renderer and every script mod | Apache License 2.0 |
| [esbuild](https://github.com/evanw/esbuild) | Linked into the `modlock` command | MIT |
