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
  rosterKey,
  rosterRows,
  Score,
  Tag,
  Text,
  TopStrip,
} from '../src/hud.js'
import {
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

  // The roster shows at most its rows.
  const wraith = heroes.find((h) => h.name === 'hero_wraith')!
  const rows = Array.from({ length: 8 }, (_, i) => ({
    name: `player ${i}`,
    self: i === 0,
    hero: i === 1 ? 0 : wraith.id,
    steamId: i === 0 ? 76561198000000001n : undefined,
    activity: 'Free roam',
  }))
  const roster = Roster({ rows })!
  expect(kids(roster)).toHaveLength(rosterRows)

  // Each row's portrait edge is accented on the viewer's own row, holds the
  // hero's portrait with the player's avatar, and is empty without a hero.
  const edge = (row: number) => kids(kids(roster)[row]!)[1]!
  expect(edge(0).props.style?.background).toBe(tones.accent)
  expect(edge(2).props.style?.background).toBe(shades.dim)
  expect(kids(kids(edge(2))[0]!)[0]!.props).toMatchObject({
    src: wraith.portrait,
  })
  expect(kids(kids(edge(1))[0]!)).toHaveLength(0)
  expect(kids(kids(edge(0))[0]!).map((child) => child.type)).toEqual([
    'image',
    'avatar',
  ])

  // A row with onPress is a button that runs it.
  let pressed = 0
  const pressable = { ...rows[2]!, onPress: () => pressed++, selected: true }
  const picked = kids(Roster({ rows: [pressable] })!)[0]!
  expect(picked.type).toBe('button')
  ;(picked.props as { onPress: (player: Player) => void }).onPress(
    new Player(1),
  )
  expect(pressed).toBe(1)

  // Only the selected row's edge takes the accent, and the key follows both.
  expect(kids(picked)[1]!.props.style?.background).toBe(tones.accent)
  expect(kids(kids(roster)[2]!)[1]!.props.style?.background).toBe(shades.dim)
  expect(rosterKey([pressable])).not.toBe(rosterKey([rows[2]!]))

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
