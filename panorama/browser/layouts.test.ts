import { expect, test } from 'bun:test'
import { actionCommand } from '../src/data.js'
import { layoutPress, scriptPath } from './layouts.js'

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
