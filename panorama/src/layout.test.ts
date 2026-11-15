// Contract tests for the layout library's press: it runs the console command
// that names the layout's mod and node, which the renderer set on its panel,
// and sends nothing it cannot name.
import { expect, test } from 'bun:test'

/** attributes are the attributes the renderer set on the layout's panel. */
const attributes = new Map<string, string>([
  ['modlock_mod', 'arena'],
  ['modlock_node', 'card'],
])

/** sent holds each event the library dispatched, with its arguments. */
const sent: unknown[][] = []

Object.assign(globalThis, {
  $: {
    GetContextPanel: () => ({
      GetAttributeString: (name: string, fallback: string) =>
        attributes.get(name) ?? fallback,
    }),
    DispatchEvent: (...args: unknown[]) => sent.push(args),
    Msg: () => {},
    RegisterForUnhandledEvent: () => {},
  },
})
const { press } = await import('./layout.js')

test('a layout presses its mod with its own node and a JSON payload', () => {
  expect(press('vote', { vote: 1 })).toBe(true)
  expect(press('close')).toBe(true)
  expect(sent).toEqual([
    ['CitadelConCommand', 'modlockaction arena card vote eyJ2b3RlIjoxfQ'],
    ['CitadelConCommand', 'modlockaction arena card close'],
  ])

  // A payload past the limit, a bad name or an unnamed layout sends nothing.
  sent.length = 0
  expect(press('keep', 'x'.repeat(200))).toBe(false)
  expect(press('a b')).toBe(false)
  attributes.delete('modlock_node')
  expect(press('close')).toBe(false)
  expect(sent).toEqual([])
})
