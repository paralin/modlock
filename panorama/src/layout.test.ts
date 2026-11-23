// Contract tests for the layout library: pressing back to its mod, watching
// data, and running unload handlers once when the renderer marks the panel.
import { expect, test } from 'bun:test'

import { layoutDataChanged, layoutUnloadingAttribute } from './data.js'

/** attributes are the attributes the renderer set on the layout's panel. */
const attributes = new Map<string, string>([
  ['modlock_mod', 'arena'],
  ['modlock_node', 'card'],
  ['modlock_data', '{"gold":1}'],
])

/** sent holds each event the library dispatched, with its arguments. */
const sent: unknown[][] = []

/** logged holds each message the library wrote to $.Msg. */
const logged: string[] = []

/** valid is what the layout's panel answers to IsValid. */
let valid = true

/** listeners holds each PlaySoundEffect callback the library registered. */
const listeners: ((sound: string) => void)[] = []

Object.assign(globalThis, {
  $: {
    GetContextPanel: () => ({
      GetAttributeString: (name: string, fallback: string) =>
        attributes.get(name) ?? fallback,
      IsValid: () => valid,
    }),
    DispatchEvent: (...args: unknown[]) => sent.push(args),
    Msg: (...values: unknown[]) => logged.push(values.join(' ')),
    RegisterForUnhandledEvent: (
      event: string,
      handler: (sound: string) => void,
    ) => {
      if (event === 'PlaySoundEffect') {
        listeners.push(handler)
      }
      return listeners.length
    },
  },
})
const { data, onUnload, press } = await import('./layout.js')

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
  attributes.set('modlock_node', 'card')
})

test('onUnload listens, runs once, survives errors and stops data', () => {
  // Runs against the same module instance as the press test; press does not
  // register a PlaySoundEffect listener.
  const wake = (sound = layoutDataChanged) => {
    for (const listener of listeners) {
      listener(sound)
    }
  }

  // Registering only onUnload still listens for the renderer's event.
  expect(listeners).toHaveLength(0)
  const calls: unknown[] = []
  onUnload(() => {
    calls.push('first')
    throw new Error('boom')
  })
  expect(listeners).toHaveLength(1)

  // Watching data shares that listener and hears updates until unload.
  data((value) => calls.push(value))
  expect(listeners).toHaveLength(1)
  attributes.set('modlock_data', '{"gold":2}')
  wake('unrelated_sound')
  expect(calls).toEqual([{ gold: 1 }])
  wake()
  expect(calls).toEqual([{ gold: 1 }, { gold: 2 }])

  // Marking unloading runs every cleanup once in order, logging a thrown error.
  onUnload(() => calls.push('second'))
  logged.length = 0
  attributes.set(layoutUnloadingAttribute, '1')
  wake()
  wake()
  attributes.set('modlock_data', '{"gold":3}')
  wake()
  expect(calls).toEqual([{ gold: 1 }, { gold: 2 }, 'first', 'second'])
  expect(logged).toEqual(['[Modlock] layout unload failed: Error: boom'])

  // A handler registered after unload runs at once; a late watch does not.
  onUnload(() => calls.push('late'))
  data((value) => calls.push(['lateData', value]))
  expect(calls).toEqual([{ gold: 1 }, { gold: 2 }, 'first', 'second', 'late'])
})

test('a wake after the panel is gone unloads without the attribute', async () => {
  // A second module instance is a second layout on its own panel.
  const fresh: string = './layout.js?gone'
  const layout = (await import(fresh)) as typeof import('./layout.js')
  const calls: unknown[] = []
  attributes.delete(layoutUnloadingAttribute)
  attributes.set('modlock_data', '{"gold":5}')
  layout.onUnload(() => calls.push('cleanup'))
  layout.data((value) => calls.push(value))
  expect(calls).toEqual([{ gold: 5 }])

  // The panel goes away unmarked; the next wake, from any layout, runs the
  // cleanup instead of reading data.
  valid = false
  attributes.set('modlock_data', '{"gold":6}')
  for (const listener of listeners) {
    listener(layoutDataChanged)
  }
  expect(calls).toEqual([{ gold: 5 }, 'cleanup'])
  valid = true
})
