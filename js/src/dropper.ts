// The dropper lets players build a spot while they play. A builder picks an
// object from a catalog; a translucent preview follows their aim and snaps
// to the surface it hits; a click drops it as a solid object everyone can
// use at once. Aiming at a placed object picks it up to move it or deletes
// it, and undo takes back the builder's own drops. Every builder drops into
// the one spot the mode holds.
//
//   const dropper = new Dropper(catalog, loadSpot(spot))
//   onStart(() => watchInput(dropperButtons | modeButtons))
//   onFrame((frame) => dropper.frame(frame.movement))
//   onInput((player, pressed) => dropper.input(player, pressed))
//   command('build', (player) => dropper.toggle(player))
//
// The keys a builder uses are withheld from their hero while they build.

import { Buttons } from './bits.js'
import {
  type Angles,
  createModel,
  type MovementSample,
  type Player,
  players,
  trace,
  type Vector,
  type WorldObject,
} from './host.gen.js'
import {
  type LoadedSpot,
  type Placed,
  type Spot,
  type SpotObject,
  triggers,
} from './spot.js'
import { hide, jsx, show, toast, type UiElement } from './ui.js'

/** CatalogEntry is one object builders can drop. */
export interface CatalogEntry {
  /** name is what builders read in the catalog. */
  readonly name: string
  /** category groups entries in the catalog, such as Crates. */
  readonly category: string
  /** model is the model's path, which the mode precached. */
  readonly model: string
  /** scale multiplies the model's size; the default is 1. */
  readonly scale?: number
  /** bounce makes the object launch heroes upward at this speed. */
  readonly bounce?: number
}

/**
 * dropperButtons are the keys a builder uses, which the mode includes in
 * watchInput: fire drops, melee picks up or puts back, parry deletes, reload
 * turns, the first two abilities page the catalog and the third undoes.
 */
export const dropperButtons =
  Buttons.attack |
  Buttons.melee |
  Buttons.parry |
  Buttons.reload |
  Buttons.ability1 |
  Buttons.ability2 |
  Buttons.ability3

/** reach is how far a builder places objects, in game units. */
const reach = 1500
/** grid is the spacing positions snap to on floors. */
const grid = 8
/** turnDegrees is how far one press of reload turns the preview. */
const turnDegrees = 15
/** floorNormal is the least upward normal that counts as a floor. */
const floorNormal = 0.7
/** previewColor tints a preview green and translucent. */
const previewColor = 0x60ff60a0
/** undoDepth is how many of a builder's drops undo remembers. */
const undoDepth = 64

/** Builder is one player building, with their preview and undo history. */
interface Builder {
  readonly player: Player
  readonly steamId: bigint
  entry: number
  yaw: number
  preview?: WorldObject
  previewModel?: string
  /** aim is where the preview stands, or nothing while it cannot drop. */
  aim?: { position: Vector; facing: Angles }
  /** target is the placed object the builder aims at. */
  target?: Placed
  /** held is a placed object the builder picked up to move. */
  held?: SpotObject
  /** drops are the builder's own drops, newest last, by entity. */
  drops: number[]
}

/** Dropper is in-game building for one mode's spot. */
export class Dropper {
  /** changed is called with the spot after every drop, move and delete. */
  changed?: (spot: Spot) => void
  /** capacity bounds how many objects the spot holds. */
  capacity = Infinity

  private builders = new Map<number, Builder>()

  constructor(
    private readonly catalog: readonly CatalogEntry[],
    public spot: LoadedSpot,
  ) {}

  /** building reports whether player is building. */
  building(player: Player): boolean {
    return this.builders.has(player.slot)
  }

  /** toggle starts or stops building for player. */
  toggle(player: Player): void {
    // A second toggle ends the player's build session.
    if (this.builders.has(player.slot)) {
      this.stop(player)
      return
    }

    // Require a connected player and a nonempty catalog.
    const connection = players().find(
      (connection) => connection.player.slot === player.slot,
    )
    if (!connection || this.catalog.length === 0) {
      return
    }

    // Start the builder facing the hero's yaw, snapped to a turn step.
    const yaw = player.pawn()?.eyeAngles.yaw ?? 0
    this.builders.set(player.slot, {
      player,
      steamId: connection.steamId,
      entry: 0,
      yaw: Math.round(yaw / turnDegrees) * turnDegrees,
      drops: [],
    })

    // Take the build buttons from the hero and show the dropper.
    player.blockInput(dropperButtons)
    this.draw(this.builders.get(player.slot)!)
  }

  /** stop ends building for player, putting back an object they held. */
  stop(player: Player): void {
    // Find the player's builder.
    const builder = this.builders.get(player.slot)
    if (!builder) {
      return
    }

    // Place any held object back, then remove the preview.
    if (builder.held) {
      this.spot.place(builder.held)
    }
    builder.preview?.remove()

    // Forget the builder and return the buttons and HUD.
    this.builders.delete(player.slot)
    player.blockInput(0n)
    hide(player)
  }

  /** replace swaps the spot builders drop into, as when a saved spot loads. */
  replace(spot: LoadedSpot): void {
    for (const builder of this.builders.values()) {
      builder.held = undefined
      builder.target = undefined
      builder.drops = []
    }
    this.spot.clear()
    this.spot = spot
  }

  /**
   * frame moves each builder's preview to their aim, drops builders who
   * left, and launches heroes standing on bouncing objects.
   */
  frame(movement: readonly MovementSample[]): void {
    const live = new Set(players().map((connection) => connection.player.slot))
    for (const builder of this.builders.values()) {
      if (live.has(builder.player.slot)) {
        this.aim(builder)
      } else {
        this.stop(builder.player)
      }
    }
    this.spot.bounce(movement)
  }

  /** input applies a builder's key presses. */
  input(player: Player, pressed: bigint): void {
    // Find the player's builder.
    const builder = this.builders.get(player.slot)
    if (!builder) {
      return
    }

    // Run the action of each pressed button.
    if (pressed & Buttons.attack) {
      this.drop(builder)
    }
    if (pressed & Buttons.melee) {
      this.pickUp(builder)
    }
    if (pressed & Buttons.parry) {
      this.delete(builder)
    }
    if (pressed & Buttons.reload) {
      this.turn(player, turnDegrees)
    }
    if (pressed & Buttons.ability1) {
      this.select(player, builder.entry - 1)
    }
    if (pressed & Buttons.ability2) {
      this.select(player, builder.entry + 1)
    }
    if (pressed & Buttons.ability3) {
      this.undo(player)
    }
  }

  /** select shows the catalog entry at index, wrapping at either end. */
  select(player: Player, index: number): void {
    const builder = this.builders.get(player.slot)
    if (!builder || builder.held) {
      return
    }
    builder.entry = (index + this.catalog.length) % this.catalog.length
    this.draw(builder)
  }

  /** turn turns player's preview by degrees. */
  turn(player: Player, degrees: number): void {
    const builder = this.builders.get(player.slot)
    if (builder) {
      builder.yaw = (builder.yaw + degrees + 360) % 360
    }
  }

  /** undo removes player's newest drop that still stands. */
  undo(player: Player): void {
    const builder = this.builders.get(player.slot)
    while (builder && builder.drops.length > 0) {
      const placed = this.spot.at(builder.drops.pop()!)
      if (placed) {
        this.spot.remove(placed)
        this.changed?.(this.spot.spot())
        return
      }
    }
  }

  /** object returns what the builder's preview stands for. */
  private object(builder: Builder): SpotObject | undefined {
    if (!builder.aim) {
      return undefined
    }
    const entry = this.catalog[builder.entry]!
    const base = builder.held ?? {
      model: entry.model,
      scale: entry.scale ?? 1,
      bounce: entry.bounce ?? 0,
      placedBy: builder.steamId,
    }
    return {
      ...base,
      position: builder.aim.position,
      facing: builder.aim.facing,
    }
  }

  /** aim places the builder's preview where they look. */
  private aim(builder: Builder): void {
    // Clear the last aim and target before tracing again.
    const pawn = builder.player.pawn()
    const model = builder.held?.model ?? this.catalog[builder.entry]!.model
    builder.aim = undefined
    builder.target = undefined
    if (!pawn) {
      return
    }

    // Trace along the hero's view to the reach distance.
    const pitch = (pawn.eyeAngles.pitch * Math.PI) / 180
    const yaw = (pawn.eyeAngles.yaw * Math.PI) / 180
    const eye = pawn.eyePosition
    const end = {
      x: eye.x + Math.cos(pitch) * Math.cos(yaw) * reach,
      y: eye.y + Math.cos(pitch) * Math.sin(yaw) * reach,
      z: eye.z - Math.sin(pitch) * reach,
    }
    const hit = trace({
      start: eye,
      end,
      exclude: triggers,
      ignore: [pawn.entity],
    })

    // Aim at the hit surface: snapped on a floor, facing out of a wall.
    if (hit && !hit.startSolid) {
      builder.target = this.spot.at(hit.entity)
      const floor = hit.normal.z >= floorNormal
      const position = floor
        ? {
            x: snap(hit.position.x),
            y: snap(hit.position.y),
            z: hit.position.z,
          }
        : hit.position
      // On a wall the object faces out of it.
      const facingYaw = floor
        ? builder.yaw
        : (Math.atan2(hit.normal.y, hit.normal.x) * 180) / Math.PI
      builder.aim = { position, facing: { pitch: 0, yaw: facingYaw, roll: 0 } }
    }

    // Show the preview at the new aim.
    this.preview(builder, model)
  }

  /** preview draws the builder's preview at their aim, or hides it. */
  private preview(builder: Builder, model: string): void {
    // Remove a preview whose model changed or that has no aim.
    if (builder.previewModel !== model || !builder.aim) {
      builder.preview?.remove()
      builder.preview = undefined
      builder.previewModel = undefined
    }

    // Without an aim there is nothing to preview.
    const aim = builder.aim
    if (!aim) {
      return
    }

    // Move an existing preview to the aim.
    if (builder.preview) {
      builder.preview.move(aim.position, aim.facing)
      return
    }

    // Create the preview model at the aim.
    builder.preview = createModel({
      resource: model,
      position: aim.position,
      facing: aim.facing,
      scale: builder.held?.scale ?? this.catalog[builder.entry]!.scale ?? 1,
      color: previewColor,
      glow: true,
    })
    builder.previewModel = model
  }

  /** drop places the preview as a solid object. */
  private drop(builder: Builder): void {
    // Build the object at the aim, or explain why there is none.
    const object = this.object(builder)
    if (!object) {
      toast(builder.player, 'Aim at a surface to drop.')
      return
    }

    // Refuse a new object when the spot is full.
    if (!builder.held && this.spot.objects().length >= this.capacity) {
      toast(
        builder.player,
        'The spot is full. Delete an object to drop another.',
      )
      return
    }

    // Place the object in the spot.
    const placed = this.spot.place(object)
    if (!placed) {
      toast(builder.player, 'That object could not be placed.')
      return
    }

    // Record the drop for undo and publish the changed spot.
    builder.held = undefined
    builder.drops = [...builder.drops, placed.entity].slice(-undoDepth)
    this.draw(builder)
    this.changed?.(this.spot.spot())
  }

  /** pickUp lifts the aimed object to move it, or puts a held one back. */
  private pickUp(builder: Builder): void {
    // A second press while holding drops the held object.
    if (builder.held) {
      this.drop(builder)
      return
    }

    // Require an aimed object to pick up.
    const target = builder.target
    if (!target) {
      return
    }

    // Lift the object out of the spot into the builder's hands.
    this.spot.remove(target)
    builder.held = target.object
    builder.yaw = target.object.facing.yaw

    // Redraw and publish the changed spot.
    this.draw(builder)
    this.changed?.(this.spot.spot())
  }

  /** delete removes the aimed object. */
  private delete(builder: Builder): void {
    if (builder.target) {
      this.spot.remove(builder.target)
      builder.target = undefined
      this.changed?.(this.spot.spot())
    }
  }

  /** draw shows the builder the catalog and their keys. */
  private draw(builder: Builder): void {
    const rows: UiElement[] = this.catalog.map((entry, index) =>
      jsx(
        'button',
        {
          style: {
            padding: [4, 10],
            background: index === builder.entry ? '#3c8c3cdd' : '#00000099',
            color: '#ffffff',
            fontSize: 16,
          },
          onPress: (player: Player) => this.select(player, index),
          children: `${entry.category} · ${entry.name}`,
        },
        index,
      ),
    )
    const held = builder.held
      ? `Moving ${builder.held.model.split('/').pop()}`
      : ''
    show(
      builder.player,
      jsx('panel', {
        style: {
          flow: 'down',
          horizontalAlign: 'right',
          verticalAlign: 'center',
          margin: [0, 24, 0, 0],
        },
        children: [
          jsx('label', {
            style: { fontSize: 22, bold: true, color: '#ffffff' },
            children: 'Object Dropper',
          }),
          jsx('label', {
            style: { fontSize: 14, color: '#dddddd' },
            children: held,
          }),
          ...windowOf(rows, builder.entry),
          jsx('label', {
            style: { fontSize: 14, color: '#dddddd', margin: [8, 0, 0, 0] },
            children:
              'Fire drop · Melee move · Parry delete · Reload turn · 1 and 2 choose · 3 undo · Tab to click',
          }),
        ],
      }),
    )
  }
}

/** windowShown is how many catalog rows show at once around the selection. */
const windowShown = 12

/** windowOf returns the rows around selected, so long catalogs stay on screen. */
function windowOf<T>(rows: readonly T[], selected: number): T[] {
  const start = Math.max(
    0,
    Math.min(selected - windowShown / 2, rows.length - windowShown),
  )
  return rows.slice(start, start + windowShown)
}

/** snap rounds a coordinate to the floor grid. */
function snap(value: number): number {
  return Math.round(value / grid) * grid
}
