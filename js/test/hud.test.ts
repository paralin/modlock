import { expect, test } from 'bun:test'

import { findHero, heroes } from '../src/heroes.js'
import { Pips, Score, Tag, Text, TopStrip } from '../src/hud.js'
import {
  objectiveBarBottom,
  pillEdge,
  teamBarBottom,
  tones,
} from '../src/theme.js'
import type { UiElement } from '../src/ui.js'

/** kids lists an element's children without the empty ones. */
function kids(element: UiElement): UiElement[] {
  const children = element.props.children
  return (Array.isArray(children) ? children : [children]).filter(
    (child): child is UiElement => typeof child === 'object' && child !== null,
  )
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
  const [block, under] = kids(strip) as [UiElement, UiElement]
  expect(block.props.style?.margin).toEqual([teamBarBottom, 0, 0, 0])
  expect(under.props.style?.margin).toEqual([objectiveBarBottom, 0, 0, 0])
  const [heading, pill] = kids(block) as [UiElement, UiElement]
  expect(heading.props.children).toBe('Round 4')
  expect(pill.props.style).toMatchObject({
    borderWidth: 2,
    borderColor: pillEdge,
  })

  // The score colors each side in its tone, without the shadow on the pill.
  const [near, , far] = kids(kids(pill)[0]!) as [
    UiElement,
    UiElement,
    UiElement,
  ]
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

  // Heroes match by game name, plain title, prefix and substring.
  expect(heroes.length).toBeGreaterThan(40)
  expect(findHero('hero_atlas')?.title).toBe('Abrams')
  expect(findHero('Lady Geist')?.name).toBe('hero_ghost')
  expect(findHero('doorman')?.title).toBe('The Doorman')
  expect(findHero('bebop')?.id).toBe(
    heroes.find((h) => h.name === 'hero_bebop')!.id,
  )
  expect(findHero('!!')).toBeUndefined()
})
