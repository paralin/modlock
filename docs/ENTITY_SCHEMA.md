# The entity system

Everything a player sees in a Deadlock match is an entity: heroes, troopers,
Guardians, pickups, zip lines, lights, sounds, the shop. Modlock gives a mod
the whole set. A mod creates any entity the server can create, reads and
writes any field the game's schema declares, and sends any input a map's
output could send, from TypeScript, Go or Luau, with types generated from the
game itself.

This document follows the system from the game's own description of its
entities to a typed call in a mod, and explains what the host checks on the
way.

## Three names for one entity

An entity has three names, and each part of the system uses one:

| Name          | Example            | Where it comes from                | What uses it                         |
| ------------- | ------------------ | ---------------------------------- | ------------------------------------ |
| Designer name | `npc_trooper_boss` | The name a map places              | `createEntity`, `create`, inputs     |
| Schema class  | `CNPC_TrooperBoss` | The C++ class the schema describes | Fields: `readField`, typed classes   |
| Subclass      | `npc_boss_tier1`   | A game data entry the entity reads | `EntityOptions.subclass`, `spawnNpc` |

`entityClass(handle)` returns the first two for a live entity. The subclass is
the data an entity reads while it spawns: a lane Guardian and a base Guardian
share a class and differ in their subclass, which gives their health, model
and weapons.

## The game dump

`modlock-host --dump DIRECTORY` starts a dedicated server, reads the game's
description of itself and quits. It writes four documents, each following
[`proto/modlock/dump/dump.proto`](../proto/modlock/dump/dump.proto) and sorted
by name so two builds diff cleanly. [`data/dump`](../data/dump) holds them for
the current game build, 6759:

| File            | Contents                                                    | Count in 6759                                                          |
| --------------- | ----------------------------------------------------------- | ---------------------------------------------------------------------- |
| `schemas.json`  | Every schema class with its base and fields, and every enum | 5,796 classes, 829 enums                                               |
| `entities.json` | Designer names, their classes, key values, inputs, outputs  | 1,183 designer names, 447 data descriptions, 1,070 inputs, 433 outputs |
| `console.json`  | Console variables and commands with their types             | 3,149 variables, 947 commands                                          |
| `survey.json`   | What creating each designer name did on a dedicated server  | 1,229 constructions                                                    |

The first three come from the game's own registries: the schema system, the
entity factories and their data descriptions, and the console. Nothing in them
is written by hand, so a game update is a new dump.

### Schema classes and fields

`schemas.json` lists each class by module. The `server` module holds the
entity classes. A field has its name, its type as the game spells it, and its
offset; Modlock keeps the name and type and finds the offset at run time, so a
game update that moves a field needs no new build of a mod.

### Data descriptions, key values and inputs

A designer name points at its data description, the table a map reads when it
places the entity. A data description adds key values and inputs to its base
and may embed others. A key value has a name, such as `rendercolor`, and a
type, such as a 32-bit color. An input has a name, such as `Color`, and the
Pulse type of its one parameter, if it takes one.

### The survey

`survey.json` answers a question the registries cannot: what happens when a
mod creates this entity on a dedicated server with nothing else around it.
A survey mod created each designer name, waited a frame and recorded the
result. A runner split the names over twelve servers and resumed after each
crash or hang from the next name.

| Result                | Names | Meaning                                                                                                        |
| --------------------- | ----: | -------------------------------------------------------------------------------------------------------------- |
| Lived                 |   578 | 542 alone, 36 only with their game data subclass                                                               |
| Refused as an ability |   520 | Abilities, items and weapons, which only a hero holds                                                          |
| Failed                |    58 | Abstract bases the server cannot create, entities that removed themselves during spawn, nodes with no identity |
| Crashed the server    |    24 | Player pawns, bot brains, path nodes, capture points and a few others                                          |
| Vanished              |     3 | Created, then gone by the next frame                                                                           |

The survey tries each name twice. The first pass gives the subclass the game's
own data suggests, when there is one. The second creates each name that lived
without its subclass, so the survey records whether the name needs it. The 36
names that need one are the units and pickups: troopers, Guardians, Walkers,
neutrals and the soul urn.

The survey checks that an entity lives one frame with default key values. It
does not check that the entity looks or behaves right.

## Generated libraries

`scripts/gen-entities.sh` runs `modlock-entitygen` over the dump and writes the
typed libraries:

| Language   | Classes and constructors                                                                       | Console                         |
| ---------- | ---------------------------------------------------------------------------------------------- | ------------------------------- |
| TypeScript | [`js/src/entities.ts`](../js/src/entities.ts), imported as `modlock/entities`                  | `modlock/console`               |
| Go         | [`mod/entity`](../mod/entity)                                                                  | [`mod/console`](../mod/console) |
| Luau       | [`luau/modlock/entities.luau`](../luau/modlock/entities.luau), required as `@modlock/entities` | `@modlock/console`              |

Each library has three parts.

**A class per server schema class.** Each field is a property in TypeScript
and Luau, and a method returning a getter and setter in Go. Each input the
class's designer names accept is a method. A class extends its base, so a
player pawn has `CBaseEntity`'s health.

**A constructor per designer name that lived.** It takes only that entity's
spawn keys, each as the type the entity reads, and returns the entity as its
class. It gives the subclass the survey found by default, so
`create('npc_trooper_boss', ...)` makes a lane Guardian without naming
`npc_boss_tier1`. A designer name that never lived has no constructor: the
type checker rejects `create('path_node', ...)` before the mod runs.

**Typed console values.** Each variable takes the type the game declares;
each command runs one line.

A bundle keeps only what it uses. The generator writes output that is stable
across runs, so a game update shows up as a readable diff of what the game
changed.

## Creating an entity

```ts
import { create } from 'modlock/entities'

const guardian = create('npc_trooper_boss', {
  team: 3,
  position,
  keys: { rendercolor: 0xff0000ff },
})
guardian?.inputAlpha(128)
guardian?.inputColor(0x00ff00ff)
```

The same call in Go:

```go
boss, err := entity.NpcTrooperBoss.Create(
	&mod.EntityOptions{Team: 3, Position: position},
	&entity.CNPC_TrooperBossKeys{},
)
_, err = entity.CBaseModelEntity(boss).InputAlpha(128)
```

And in Luau:

```luau
local boss: entities.CNPC_TrooperBoss? = entities.create("npc_trooper_boss", { team = 3, position = position })
```

Underneath, each is one `createEntity` call, which a mod may also make by
hand with any designer name and untyped key values. The host runs it in this
order:

1. It checks that the placement is finite.
2. It resolves the designer name's entity class through the game's own class
   lookup. A name the server lacks fails here.
3. It refuses an ability, item or weapon: any class deriving from
   `CCitadelBaseAbility`. One created alone, with no hero to hold it, takes the
   server down once it thinks or is removed, so the refusal happens before the
   entity exists. The message points at `giveItem` and `replaceAbility`, which
   give one to a hero.
4. It creates the entity, installs its subclass, and applies the spawn key
   values, the position and the facing.
5. It writes the fields in `EntityOptions.fields`. These are fields the game
   reads only while spawning, such as a unit's `m_iInitialTeamNum`. A write
   that fails removes the entity unspawned and fails the call.
6. It spawns the entity and returns its handle.

The host records every entity a mod creates. The world removes them when it
ends, and stopping or reloading the mod removes them too, so a mod never
leaves entities behind.

### Units and pickups

`spawnNpc` and `createPickup` are built on the same call. `spawnNpc` takes any
unit class the server has, with its subclass as `unit`, and adds what a unit
needs: health and maximum health, and a lane. A lane is written to the class's
`m_iLane` field before the unit spawns; a class that walks no lane, such as
`npc_yakuza_gangster`, refuses one with a message naming it. `Npc` reads,
moves, heals and removes the unit.

`createPickup` makes the soul urn or the movement buff with the designer name
and subclass each needs. `Pickup` reports whether it still lies in the world
and removes it.

## Fields

A mod reads and writes a field of any live entity by class and name:

```ts
import { readField, writeField } from 'modlock'

const health = readField(pawn.entity, 'CBaseEntity', 'm_iHealth', 'int32')
writeField(pawn.entity, 'CBaseEntity', 'm_iHealth', 'int32', 500)
```

or through a typed class, which supplies the class, name and type:

```ts
import { CCitadelPlayerPawn } from 'modlock/entities'

const hero = new CCitadelPlayerPawn(pawn.entity)
hero.m_iHealth = (hero.m_iHealth ?? 0) + 50
```

The host finds the field's offset through the schema each time the mod first
names it, and caches it. Before it touches memory, it checks that the entity is
of the named class or derives from it. A handle to a trooper named as a
`CCitadelPlayerPawn` is refused with both class names, and nothing is written.
A write marks the field changed so the server sends it to the players.

Field types cover booleans, signed and unsigned integers of 8 to 64 bits, 32
and 64-bit floats, vectors, entity handles and strings. Angles travel as
vectors. A typed field whose type is another entity class reads as that
class, so `hero.m_hController` is a controller a mod can read further.

## Inputs

`fireInput(handle, input, value)` sends an entity an input, as a map's output
would: `Kill`, `Color`, `Alpha`, `Skin`, `SetScale`, `StartShake`, `Explode`
and the thousand others in `entities.json`. An input reads its value as the
type it takes and does not convert text, so `Alpha` takes an integer and
`Color` a color as `0xRRGGBBAA`. A typed class sends each input as a method
whose parameter has that type:

```ts
const shake = create('env_shake', {
  position,
  keys: { amplitude: 12, frequency: 40, duration: 2, radius: 2000 },
})
shake?.inputStartShake()

const boom = create('env_explosion', {
  position,
  keys: { iMagnitude: 50, iRadiusOverride: 200 },
})
boom?.inputExplode()
```

An input the entity lacks fails that call alone with the entity's designer
name and the input's.

## What fails, and how

Every entity call fails on its own: it logs, returns `undefined` or `false`
in TypeScript and Luau and an error in Go, and the mod keeps running. The
failures a mod can meet:

| Message                                       | Cause                                                        |
| --------------------------------------------- | ------------------------------------------------------------ |
| `the server has no entity NAME`               | The designer name is not one this build knows                |
| `NAME is an ability, which only a hero holds` | An ability, item or weapon; give it to a hero instead        |
| `NAME crashes the server when created alone`  | One of the entities below; the message says what it needs    |
| `NAME walks no lane`                          | `spawnNpc` was given a lane for a class without `m_iLane`    |
| `NAME has no native identity`                 | The entity was created without a handle the host can address |
| `NAME has no input INPUT`                     | The input is not one the entity accepts                      |
| A class mismatch naming both classes          | A field was named on a class the entity does not derive from |

The 24 designer names that crashed the survey have no constructor, and
`create` refuses them before they exist, along with `citadel_bounce_pad`. The
survey created a bounce pad safely, but the pad finds its team through the
ability that placed it, and the first hero to cross a pad made alone crashes
the server. Each of these reads state that a map, a game mode, an owning hero
or the ability that makes it provides:

| Needs                      | Designer names                                                                                                                                          |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| The ability that makes it  | `citadel_bounce_pad`, `citadel_deployable_preview`, `citadel_magician_turret_object`, `citadel_mobile_resupply_object`, `citadel_nano_predatory_statue` |
| An owner                   | `citadel_hideout_prop_base`, `citadel_trigger_capture_zipline`, `npc_familiar_helper`, `npc_shielded_sentry`                                            |
| Its map's game data        | `citadel_herotest_orbspawner`, `citadel_hideout_shootable_target_spawner`, `simple_animating_ai`, `env_laser`, `point_prefab`                           |
| A model or path from a map | `func_precipitation`, `spark_shower`, `path_node`, `path_node_mover`                                                                                    |
| The hideout map            | `npc_neutral_hideout_cat`, `npc_neutral_hideout_rabbit`                                                                                                 |
| A game mode's capture data | `citadel_capture_point`, `citadel_multi_capture_point`                                                                                                  |
| A player or bot            | `baseplayerpawn` (spawn a hero), `npc_player_bot_brain` (add a bot)                                                                                     |
| Joined objects             | `physics_npc_solver`                                                                                                                                    |

## Updating for a new game build

After a game update:

1. Run `modlock-host --dump data/dump` on a Windows server, or under Proton.
2. Run the survey and replace `data/dump/survey.json`.
3. Run `scripts/gen-entities.sh` and review the diff of the generated
   libraries, which shows what the update changed.

A mod built against an earlier dump keeps working when its fields and inputs
survive the update, because the host resolves them by name at run time.
