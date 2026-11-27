import { expect, test } from 'bun:test'
import { actionCommand } from '../src/data.js'
import {
  type Compile,
  fillPlaces,
  LayoutScope,
  layoutPress,
  localize,
  scriptPath,
} from './layouts.js'

test('a layout press reads back from the command the library writes', () => {
  // A press with a payload and one without come back as sent.
  const payload = JSON.stringify({ route: 'gate', note: 'é→' })
  expect(layoutPress(actionCommand('arena', 'card', 'retry', payload))).toEqual(
    { mod: 'arena', node: 'card', action: 'retry', payload },
  )
  expect(layoutPress(actionCommand('arena', 'card', 'close', ''))).toEqual({
    mod: 'arena',
    node: 'card',
    action: 'close',
    payload: '',
  })

  // Another command, a short one and a payload that is not UTF-8 read as none.
  expect(layoutPress('say hello')).toBeUndefined()
  expect(layoutPress('modlockaction arena card')).toBeUndefined()
  expect(layoutPress('modlockaction arena card keep _w')).toBeUndefined()
  expect(layoutPress('modlockaction arena card keep a+b')).toBeUndefined()
})

test('a layout script loads from the page', () => {
  // The layout library is the page's copy; a mod's script is its build's.
  expect(
    scriptPath('s2r://panorama/scripts/modlock/layout.vjs', '?build=2'),
  ).toBe('/layout.js')
  expect(
    scriptPath('file://{resources}/scripts/modlock/layout.js', '?build=2'),
  ).toBe('/layout.js')
  expect(
    scriptPath('s2r://panorama/scripts/arena-1a2b/board.vjs', '?build=2'),
  ).toBe('/scripts/arena-1a2b/board.js?build=2')

  // A game path the page does not serve has none.
  expect(scriptPath('s2r://panorama/scripts/board.vjs', '')).toBe('')
  expect(scriptPath('file://{resources}/layout/arena/board.xml', '')).toBe('')
})

test("a layout's events bind in its own script scope", () => {
  // Two layouts, an outer one and one nested inside it, each compile an
  // event's code with their own scope.
  const outer = new LayoutScope()
  const inner = new LayoutScope()
  const ran: string[] = []
  const scoped =
    (layout: string): Compile =>
    (code) =>
    () => {
      ran.push(`${layout}:${code}`)
    }
  const handlers: (() => void)[] = []
  const set = (handler: () => void) => {
    handlers.push(handler)
  }

  // The outer layout's scripts run before the nested layout's panels are
  // built, and the nested layout's events wait for its own scripts.
  outer.bind('Open()', set)
  outer.ready(scoped('outer'))
  inner.bind('RollDice()', set)
  expect(handlers).toHaveLength(1)
  inner.ready(scoped('inner'))
  inner.ready(scoped('outer'))
  inner.bind('Close()', set)

  // Each handler runs its code in the scope of the layout that declared it.
  for (const handler of handlers) handler()
  expect(ran).toEqual(['outer:Open()', 'inner:RollDice()', 'inner:Close()'])
})

test('a label fills its dialog variable places', () => {
  // A place with a value fills, and one without stays as written.
  const values: Record<string, string> = { score: '3', hero: '7' }
  const lookup = (name: string) => values[name]
  expect(fillPlaces('{d:score} / {s:goal}', lookup)).toBe('3 / {s:goal}')
  expect(fillPlaces('{g:citadel_hero_name:hero}', lookup)).toBe('hero 7')

  // A bare token shows the panel's own values, and one with places fills them.
  expect(localize('#hud_score', { score: '3' }, lookup)).toBe('#hud_score 3')
  expect(localize('#hud_score', {}, lookup)).toBe('#hud_score')
  expect(localize('Score {d:score}', { score: '3' }, lookup)).toBe('Score 3')
})
