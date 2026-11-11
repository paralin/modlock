// RaceGate stands a checkpoint or finish gate across a lane: a window of thin
// glowing lines inside a racing checker band, round the window's sides and
// top, in Tron cyan by default. A runner reads it at once as "go through
// here":
//
//   const gate = RaceGate.stand({ foot: ground(spot) ?? spot, yaw: 90 })
//   gate.remove()
//
// The band is world text: long strings of square glyphs, which keep drawing
// from far off where single glyphs stop. Each strip draws four times, offset
// a little across and up, so the squares fill their cells and touch at the
// corners, and each copy stands a step deeper than the last so the copies do
// not flicker against each other. The window is engine beams between named
// points. World text and beams ignore alpha, so the band's outer ring fakes a
// see-through glow with a darker shade.

import {
  createEntity,
  createParticle,
  createText,
  fireInput,
  removeEntity,
  showEntityOnlyTo,
  type Vector,
  type WorldObject,
} from './host.gen.js'

/** RaceGateOptions place a gate and choose its look; most have a default. */
export interface RaceGateOptions {
  /** foot is the road point under the middle of the gate's opening. */
  readonly foot: Vector
  /** yaw is the direction a runner passes through the gate, in degrees. */
  readonly yaw: number
  /** width is the opening's width, 112 units by default, in whole cells. */
  readonly width?: number
  /** height is the opening's height, 144 units by default, in whole cells. */
  readonly height?: number
  /** color lights the window and the band, 0xRRGGBBAA, Tron cyan by default. */
  readonly color?: number
  /** faded colors the band's outer ring, a darker cyan by default. */
  readonly faded?: number
  /** viewer is the one player slot that sees the gate; everyone when unset. */
  readonly viewer?: number
}

/** cell is the side of one checker square, in world units. */
const cell = 8

/** margin is how many cells deep the band is round the window. */
const margin = 3

/** block fills a checker cell: U+25A0, a square glyph. */
const block = '■'

/** gap is an empty cell: the full-width space, as wide as the block. */
const gap = '　'

/** blockScale gives the block glyph an advance of one cell. */
const blockScale = 4.05

/** inkSpread grows the glyph's ink to its cell's edges, in world units. */
const inkSpread = 0.6

/**
 * inkDepth parts the four copies of a strip in depth, enough that the depth
 * buffer keeps them apart instead of flickering between them.
 */
const inkDepth = 0.15

/** columnRise aligns a turned column's glyph origin with its cell's edge. */
const columnRise = 1.75

/** rowAcross aligns a centered row's ink with its cells. */
const rowAcross = -1.2

/** rowRise centers a row's ink on its cells. */
const rowRise = 0.5

/** frameWidth is the width of the window's outer line. */
const frameWidth = 2

/** railInset is how far the window's inner rail runs inside its outer line. */
const railInset = 8

/** railAlpha is the inner rail's opacity, of 0xff. */
const railAlpha = 0x60

/** tron is the default glow, a bright cyan. */
const tron = 0x3ef4ffff

/** tronFaded is the default outer ring, a dark cyan. */
const tronFaded = 0x14707aff

/** anchorParticle draws nothing; a beam's end rides it. */
const anchorParticle = 'particles/generic/null.vpcf'

/**
 * beamMaterial is the flat material the window draws with. The game loads it
 * in every world, so a gate draws without a precache.
 */
const beamMaterial = 'materials/physics/wireframe_basic.vmat'

/** raceGateResources are what a gate draws with, for a mod's precache. */
export const raceGateResources = [anchorParticle, beamMaterial]

/** nextPoint numbers the beams' named points, keeping each name unique. */
let nextPoint = 0

/** RaceGate is a gate standing in the world, which remove takes down. */
export class RaceGate {
  private constructor(
    private readonly objects: WorldObject[],
    private readonly entities: number[],
  ) {}

  /**
   * stand stands a gate across yaw at foot. A part the world refuses is left
   * out, so the gate stands whatever the world made.
   */
  static stand(options: RaceGateOptions): RaceGate {
    // Size the opening in whole cells, and face its plane across the lane.
    const radians = (options.yaw * Math.PI) / 180
    const halfWidth = Math.round((options.width ?? 112) / cell / 2) * cell
    const height = Math.round((options.height ?? 144) / cell) * cell
    const color = options.color ?? tron
    const plane: Plane = {
      foot: options.foot,
      yaw: radians,
      across: { x: -Math.sin(radians), y: Math.cos(radians) },
    }

    // Draw the window's outer line and inner rail, then the band round them.
    const made: Made = { objects: [], entities: [] }
    const rail = ((color & ~0xff) | railAlpha) >>> 0
    outline(plane, halfWidth, height, 0, color, frameWidth, made)
    outline(plane, halfWidth, height, railInset, rail, frameWidth / 2, made)
    band(plane, halfWidth, height, color, options.faded ?? tronFaded, made)

    // Show the gate to its one viewer, when it has one.
    if (options.viewer !== undefined) {
      const viewers = [options.viewer]
      made.objects.forEach((object) => object.showOnlyTo(viewers))
      made.entities.forEach((entity) => showEntityOnlyTo(entity, viewers))
    }
    return new RaceGate(made.objects, made.entities)
  }

  /** remove takes every part of the gate out of the world. */
  remove(): void {
    this.entities.forEach((entity) => removeEntity(entity))
    this.objects.forEach((object) => object.remove())
    this.objects.length = 0
    this.entities.length = 0
  }
}

/** Made collects the parts a gate stands, for remove to take down. */
interface Made {
  /** objects are the band's texts and the beams' carrier particles. */
  readonly objects: WorldObject[]
  /** entities are the beams and their named points. */
  readonly entities: number[]
}

/** Plane is the gate's plane: its foot, its yaw in radians, and its across. */
interface Plane {
  readonly foot: Vector
  readonly yaw: number
  readonly across: { readonly x: number; readonly y: number }
}

/**
 * at places a point side units across the gate, up units over its foot and
 * deep units past it along the lane.
 */
function at(plane: Plane, side: number, up: number, deep = 0): Vector {
  return {
    x: plane.foot.x + side * plane.across.x + Math.cos(plane.yaw) * deep,
    y: plane.foot.y + side * plane.across.y + Math.sin(plane.yaw) * deep,
    z: plane.foot.z + up,
  }
}

/**
 * band stands the checker band round the window: long columns down its
 * sides and long rows over its top, every other cell filled, into made.
 */
function band(
  plane: Plane,
  halfWidth: number,
  height: number,
  lit: number,
  faded: number,
  made: Made,
): void {
  // Count the cells: the band reaches margin cells past the sides and top.
  const width = halfWidth + margin * cell
  const columns = (width * 2) / cell
  const openingRows = height / cell
  const rows = openingRows + margin

  // Each side column runs from under the outer row to the road. It starts on
  // the cell its checker parity fills, so the pattern runs round the corners.
  for (let column = 0; column < columns; column++) {
    if (column >= margin && column < columns - margin) continue
    const first = rows - 2 - ((column + rows - 2) % 2)
    const count = Math.floor(first / 2) + 1
    const outer = column === 0 || column === columns - 1
    strip(plane, {
      text: Array(count).fill(block).join(gap),
      side: -width + (column + 0.5) * cell,
      up: (first + 1) * cell + columnRise,
      pitch: 90,
      color: outer ? faded : lit,
      made,
    })
  }

  // Each row over the opening runs between the side columns; the outer row
  // spans the whole band.
  for (let row = openingRows; row < rows; row++) {
    const outer = row === rows - 1
    const start = outer ? 0 : margin
    const end = outer ? columns : columns - margin
    const first = start + ((start + row) % 2)
    const count = Math.ceil((end - first) / 2)
    strip(plane, {
      text: Array(count).fill(block).join(gap),
      side: -width + (first + count - 0.5) * cell + rowAcross,
      up: (row + 0.5) * cell + rowRise,
      pitch: 0,
      color: outer ? faded : lit,
      made,
    })
  }
}

/** Strip is one string of the band and where it stands. */
interface Strip {
  /** text is the strip's blocks and gaps. */
  readonly text: string
  /** side is how far across the gate the strip stands. */
  readonly side: number
  /** up is how high over the foot the strip stands. */
  readonly up: number
  /** pitch turns the strip in the gate's plane: 90 reads down, 0 across. */
  readonly pitch: number
  /** color is the strip's color, 0xRRGGBBAA. */
  readonly color: number
  /** made collects the strip's texts. */
  readonly made: Made
}

/**
 * strip stands a strip as four copies, offset by inkSpread across and up so
 * the ink fills each cell, each copy a step of inkDepth deeper.
 */
function strip(
  plane: Plane,
  { text, side, up, pitch, color, made }: Strip,
): void {
  const yaw = (plane.yaw * 180) / Math.PI - 90
  for (const dx of [-1, 1]) {
    for (const dy of [-1, 1]) {
      const shown = createText({
        text,
        position: at(
          plane,
          side + dx * inkSpread,
          up + dy * inkSpread,
          (inkDepth * (dx + 2 * dy + 3)) / 2,
        ),
        facing: { pitch, yaw, roll: 90 },
        fontSize: 100,
        scale: blockScale,
        color,
        align: pitch === 90 ? 'left' : 'center',
        verticalAlign: 'center',
      })
      if (shown) made.objects.push(shown)
    }
  }
}

/**
 * outline strings beams round the window's four corners, inset from its
 * outer line, into made.
 */
function outline(
  plane: Plane,
  halfWidth: number,
  height: number,
  inset: number,
  color: number,
  width: number,
  made: Made,
): void {
  // Name one point at each corner, shared by the two beams that meet there.
  const corners = [
    at(plane, -(halfWidth - inset), inset),
    at(plane, halfWidth - inset, inset),
    at(plane, halfWidth - inset, height - inset),
    at(plane, -(halfWidth - inset), height - inset),
  ]
  const names = corners.map((corner) => anchor(corner, made))
  if (names.some((name) => name === undefined)) return

  // Join each corner to the next, the last back to the first.
  corners.forEach((corner, index) => {
    const beam = createEntity({
      designerName: 'env_beam',
      subclass: '',
      team: 0,
      position: corner,
      keyValues: [
        { key: 'LightningStart', value: { text: names[index]! } },
        { key: 'LightningEnd', value: { text: names[(index + 1) % 4]! } },
        { key: 'texture', value: { text: beamMaterial } },
        { key: 'BoltWidth', value: { number: width } },
        { key: 'NoiseAmplitude', value: { number: 0 } },
        { key: 'TextureScroll', value: { integer: 0 } },
        { key: 'life', value: { number: 0 } },
        { key: 'rendercolor', value: { color } },
        { key: 'renderamt', value: { integer: 255 } },
        { key: 'spawnflags', value: { integer: 1 } },
      ],
    })
    if (beam) made.entities.push(beam)
  })
}

/**
 * anchor spawns a named point at position for a beam's end, riding an empty
 * particle, and returns its name. Both parts join made.
 */
function anchor(position: Vector, made: Made): string | undefined {
  // Spawn the empty particle the point rides.
  const carrier = createParticle({ resource: anchorParticle, position })
  const host = carrier?.entity()
  if (!carrier || !host) {
    carrier?.remove()
    return undefined
  }
  made.objects.push(carrier)

  // Name the point uniquely past a reload, and set it on the particle.
  const name = `racegate_${host}_${nextPoint++}`
  const point = createEntity({
    designerName: 'info_particle_system',
    subclass: '',
    team: 0,
    position,
    keyValues: [{ key: 'targetname', value: { text: name } }],
  })
  if (!point) return undefined
  made.entities.push(point)
  fireInput(point, 'SetParent', { text: '!activator' }, host)
  return name
}
