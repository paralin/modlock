import { expect, test } from 'bun:test'

import {
  clock,
  count,
  grouped,
  ordinal,
  percent,
  stopwatch,
} from '../src/format.js'
import { Player } from '../src/host.gen.js'
import { findHero, heroes } from '../src/heroes.js'
import {
  Pips,
  Roster,
  rosterBottom,
  rosterKey,
  type RosterRow,
  Score,
  Tag,
  Text,
  TopStrip,
} from '../src/hud.js'
import {
  fpsReadoutBottom,
  objectiveBarBottom,
  pillEdge,
  shades,
  teamBarBottom,
  tones,
} from '../src/theme.js'
import type { Child, UiElement } from '../src/ui.js'
import {
  aimAt,
  distance,
  flatDistance,
  forward,
  toward,
} from '../src/vector.js'

/** isElement reports whether child is an element. */
function isElement(child: Child): child is UiElement {
  return typeof child === 'object' && child !== null && !Array.isArray(child)
}

/** isList reports whether child is a list; Array.isArray narrows to any[]. */
function isList(child: Child): child is readonly Child[] {
  return Array.isArray(child)
}

/** kids lists an element's children without the empty ones. */
function kids(element: UiElement): UiElement[] {
  const children = element.props.children
  const found: UiElement[] = []
  if (isList(children)) {
    for (const child of children) if (isElement(child)) found.push(child)
  } else if (isElement(children)) {
    found.push(children)
  }
  return found
}

test('the HUD kit draws in its tokens and finds heroes by typed name', () => {
  // A text takes its role's face, size and capitals over the shadow.
  const label = Text({ role: 'label', children: 'Round 2' })
  expect(label.type).toBe('label')
  expect(label.props.style).toMatchObject({
    face: 'block',
    fontSize: 15,
    uppercase: true,
    shadow: true,
    color: tones.muted,
  })

  // Keeping the team bar seats the heading and pill under it, and the line
  // under the objective bar.
  const strip = TopStrip({
    heading: 'Round 4',
    line: '0:42',
    keepTeamBar: true,
    children: Score({ points: [3, 1], tones: ['amber', 'sapphire'] }),
  })
  const block = kids(strip)[0]!
  const under = kids(strip)[1]!
  expect(block.props.style?.margin).toEqual([teamBarBottom, 0, 0, 0])
  expect(under.props.style?.margin).toEqual([objectiveBarBottom, 0, 0, 0])
  const heading = kids(block)[0]!
  const pill = kids(block)[1]!
  expect(heading.props.children).toBe('Round 4')
  expect(pill.props.style).toMatchObject({
    borderWidth: 2,
    borderColor: pillEdge,
  })

  // The score colors each side in its tone, without the shadow on the pill.
  const near = kids(kids(pill)[0]!)[0]!
  const far = kids(kids(pill)[0]!)[2]!
  expect(near.props.style?.color).toBe(tones.amber)
  expect(far.props.style?.color).toBe(tones.sapphire)
  expect(near.props.style?.shadow).toBe(false)

  // In the bar's place one column holds it all; bare, only the pill draws.
  expect(kids(TopStrip({ heading: 'Arena', line: '0:42' }))).toHaveLength(3)
  expect(kids(TopStrip({}))).toHaveLength(1)

  // Pips fill from the near side, or the far side when mirrored.
  const lit = (element: UiElement) =>
    kids(element).map((pip) => pip.props.style?.background === tones.gold)
  expect(lit(Pips({ filled: 1, total: 3, tone: 'gold' }))).toEqual([
    true,
    false,
    false,
  ])
  expect(
    lit(Pips({ filled: 1, total: 3, tone: 'gold', mirror: true })),
  ).toEqual([false, false, true])

  // A filled tag sets its words on its tone.
  expect(
    Tag({ tone: 'gold', fill: 'filled', children: 'MVP' }).props.style,
  ).toMatchObject({
    background: tones.gold,
    color: tones.onAccent,
  })

  // The roster shows every row while they fit, at full size.
  const wraith = heroes.find((h) => h.name === 'hero_wraith')!
  const rows = Array.from({ length: 4 }, (_, i) => ({
    key: i,
    name: `player ${i}`,
    self: i === 0,
    hero: i === 1 ? 0 : wraith.id,
    steamId: i === 0 || i === 1 ? 76561198000000001n : undefined,
    activity: 'Free roam',
  }))
  const roster = Roster({ rows })!
  // The groups stand in the roster's last column, after any menu.
  const column = (r: UiElement) => kids(kids(r).at(-1)!)
  const group = (r: UiElement, at = 0) => kids(column(r)[at]!)
  expect(group(roster)).toHaveLength(4)

  // Each row's portrait edge is accented on the viewer's own row, holds the
  // hero's portrait with the player's avatar, the avatar alone without a
  // hero, and the hero alone without an account.
  const edge = (row: number) => kids(group(roster)[row]!)[1]!
  expect(edge(0).props.style?.width).toBe(44)
  expect(edge(0).props.style?.background).toBe(tones.accent)
  expect(edge(2).props.style?.background).toBe(shades.dim)
  expect(kids(kids(edge(2))[0]!)[0]!.props).toMatchObject({
    src: wraith.portrait,
  })
  expect(kids(kids(edge(0))[0]!).map((child) => child.type)).toEqual([
    'image',
    'avatar',
  ])
  expect(kids(kids(edge(1))[0]!).map((child) => child.type)).toEqual(['avatar'])
  expect(kids(kids(edge(3))[0]!).map((child) => child.type)).toEqual(['image'])
  expect(group(roster)[2]!.key).toBe('row-2')

  // A row with onPress is a button that runs it.
  let pressed = 0
  const pressable = { ...rows[2]!, onPress: () => pressed++, selected: true }
  const picked = group(Roster({ rows: [pressable] })!)[0]!
  expect(picked.type).toBe('button')
  ;(picked.props as { onPress: (player: Player) => void }).onPress(
    new Player(1),
  )
  expect(pressed).toBe(1)

  // Only the selected row's edge takes the accent, and the key follows both.
  expect(kids(picked)[1]!.props.style?.background).toBe(tones.accent)
  // RosterRow, not the narrower type of rows: scriptc copies a record into a
  // narrower shape and drops the fields that shape lacks.
  const one = (row: RosterRow) => [{ key: 'p', rows: [row] }]
  expect(rosterKey(one(pressable))).not.toBe(rosterKey(one(rows[2]!)))

  // Groups head their rows, and empty groups draw nothing. As players join,
  // rows shrink, then run in columns, and every player stays on screen above
  // the minimap.
  const team = (n: number, from: number) =>
    Array.from({ length: n }, (_, i) => ({
      key: from + i,
      name: `player ${from + i}`,
      hero: wraith.id,
      out: i === 0,
    }))
  const teams = (seekers: number, hiders: number) =>
    Roster({
      groups: [
        {
          key: 'seekers',
          title: 'Seekers',
          tone: 'amber',
          rows: team(seekers, 0),
        },
        {
          key: 'hiders',
          title: 'Hiders',
          tone: 'sapphire',
          rows: team(hiders, 100),
        },
        { key: 'watching', title: 'Watching', rows: [] },
      ],
    })!
  const small = teams(2, 6)
  expect(column(small)).toHaveLength(2)
  expect(kids(kids(column(small)[0]!)[0]!)[0]!.props.children).toBe('Seekers')
  const portraitOf = (r: UiElement) => {
    const first = group(r)[1]!
    const row = first.key?.startsWith('line-') ? kids(first)[0]! : first
    return kids(row)[1]!.props.style?.width
  }
  expect(portraitOf(small)).toBe(44)
  expect(portraitOf(teams(3, 9))).toBe(32)
  const crowd = teams(8, 24)
  expect(portraitOf(crowd)).toBe(24)
  expect(group(crowd, 1)[1]!.key).toBe('line-0')
  const shown = (r: UiElement) =>
    column(r)
      .flatMap((g) => kids(g).filter(Boolean).slice(1))
      .flatMap((l) => (l.key?.startsWith('line-') ? kids(l) : [l]))
  expect(shown(crowd)).toHaveLength(32)
  // An out player's row dims.
  expect(shown(crowd)[0]!.props.style?.opacity).toBe(0.55)

  // A row's menu hangs left of the column, its top level with its row's:
  // past the first group's heading and its rows, the second's gap, heading
  // and the lines above the row.
  const menu = Text({ role: 'label', children: 'menu' })
  const opened = Roster({
    groups: [
      { key: 'seekers', title: 'Seekers', rows: team(2, 0) },
      {
        key: 'hiders',
        title: 'Hiders',
        rows: team(6, 100).map((row, i) => (i === 3 ? { ...row, menu } : row)),
      },
    ],
  })!
  const [hang, beside] = kids(opened)
  expect(opened.props.style?.flow).toBe('right')
  expect(hang!.key).toBe('menu')
  expect(beside!.key).toBe('column')
  expect(kids(hang!)[0]).toBe(menu)
  expect(hang!.props.style?.margin).toEqual([
    24 + 2 * 52 + 10 + 24 + 3 * 52,
    8,
    0,
    0,
  ])

  // A band too short for every player keeps a line for each group and counts
  // the rest.
  const cramped = Roster({
    top: 100,
    bottom: 300,
    groups: [
      { key: 'seekers', title: 'Seekers', rows: team(8, 0) },
      { key: 'hiders', title: 'Hiders', rows: team(24, 100) },
    ],
  })!
  const rest = shown(cramped).filter((cell) => cell.key === 'rest')
  expect(rest).toHaveLength(2)
  expect(shown(cramped).length < 32).toBe(true)

  // A panel under the roster starts where its rows end, inside the band.
  const crampedEnd = rosterBottom({
    top: 100,
    bottom: 300,
    groups: [
      { key: 'seekers', title: 'Seekers', rows: team(8, 0) },
      { key: 'hiders', title: 'Hiders', rows: team(24, 100) },
    ],
  })
  expect(crampedEnd <= 288).toBe(true)
  expect(rosterBottom({ rows: team(4, 0) })).toBe(fpsReadoutBottom + 4 * 52)
  expect(rosterBottom({ top: 90, rows: [] })).toBe(90)

  // Without rows the roster draws nothing.
  expect(Roster({ rows: [] })).toBeNull()

  // Heroes match by game name, plain title, prefix and substring.
  expect(heroes.length).toBeGreaterThan(40)
  expect(findHero('hero_atlas')?.title).toBe('Abrams')
  expect(findHero('Lady Geist')?.name).toBe('hero_ghost')
  expect(findHero('doorman')?.title).toBe('The Doorman')
  expect(findHero('bebop')?.id).toBe(
    heroes.find((h) => h.name === 'hero_bebop')!.id,
  )
  expect(findHero('!!')).toBeUndefined()

  // HUD numbers read as players read them, without the missing locale data.
  expect([clock(-1), clock(0.2), clock(247), stopwatch(64.379)]).toEqual([
    '0:00',
    '0:01',
    '4:07',
    '1:04.37',
  ])
  expect([percent(1, 3), percent(5, 4), percent(1, 0)]).toEqual([
    '33%',
    '100%',
    '-',
  ])
  expect([grouped(4210.4), grouped(-1234567), grouped(999)]).toEqual([
    '4,210',
    '-1,234,567',
    '999',
  ])
  expect([1, 2, 3, 4, 11, 12, 13, 21, 112].map(ordinal)).toEqual([
    '1st',
    '2nd',
    '3rd',
    '4th',
    '11th',
    '12th',
    '13th',
    '21st',
    '112th',
  ])
  expect([
    count(1, 'kill'),
    count(1200, 'kill'),
    count(2, 'ally', 'allies'),
  ]).toEqual(['1 kill', '1,200 kills', '2 allies'])

  // A view aimed at a point looks along the direction to it.
  const eye = { x: 0, y: 0, z: 64 }
  const target = { x: 100, y: 100, z: 0 }
  const look = forward(aimAt(eye, target))
  const along = toward(eye, target)
  expect(look.x).toBeCloseTo(along.x)
  expect(look.y).toBeCloseTo(along.y)
  expect(look.z).toBeCloseTo(along.z)

  // Aiming down at a point to the northeast turns to 45 degrees and looks down.
  expect(aimAt(eye, target).yaw).toBeCloseTo(45)
  expect(aimAt(eye, target).pitch).toBeGreaterThan(0)

  // Distances ignore height when flat, and a point is no step from itself.
  expect(flatDistance(eye, target)).toBeCloseTo(Math.SQRT2 * 100)
  expect(distance(eye, eye)).toBe(0)
  expect(toward(eye, eye)).toEqual({ x: 0, y: 0, z: 0 })
})
