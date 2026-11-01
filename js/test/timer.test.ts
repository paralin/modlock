import { expect, test } from 'bun:test'

import { Reply } from '../../src/wasm/wasm.pb.js'
import { bind } from '../src/host.js'
import { advance, after, every, sequence } from '../src/timer.js'

// The timers log a failing callback through the host bridge.
const logged: Uint8Array[] = []
bind((call) => {
  logged.push(call)
  return Reply.toBinary({})
})

/** clock is the game time of the latest frame the tests advanced to. */
let clock = 100

/** frame advances the game clock by seconds and runs the timers that are due. */
function frame(seconds: number): void {
  clock += seconds
  advance(clock)
}

/** count returns how many of the runs are named name. */
function count(runs: string[], name: string): number {
  return runs.filter((run) => run === name).length
}

test('after runs once, on time, and every runs on its cadence', () => {
  // The timers start counting at the first frame.
  const runs: string[] = []
  frame(0)
  after(2, () => runs.push('after'))
  every(1, () => runs.push('every'))

  // Nothing is due until a second has passed, then the repeat runs.
  frame(0.5)
  expect(runs).toEqual([])
  frame(0.5)
  expect(runs).toEqual(['every'])

  // A late frame runs each timer once.
  frame(1.2)
  expect(runs).toEqual(['every', 'after', 'every'])

  // The late frame at 102.2 ran the repeat for 102, so the next is due at 103.
  frame(0.7)
  expect(count(runs, 'every')).toBe(2)
  frame(0.2)
  expect(count(runs, 'every')).toBe(3)

  // A new map's clock starts over, and the repeat keeps its remaining delay.
  clock = 0
  frame(0)
  frame(0.8)
  expect(count(runs, 'every')).toBe(3)
  frame(0.2)
  expect(count(runs, 'every')).toBe(4)
})

test('a cancelled timer never runs and reports finished', () => {
  // A repeat runs once, then is cancelled.
  let runs = 0
  frame(0)
  const timer = every(1, () => runs++)
  frame(1)
  expect(runs).toBe(1)
  timer.cancel()

  // The cancelled timer stays quiet.
  expect(timer.finished).toBe(true)
  frame(5)
  expect(runs).toBe(1)
})

test('a sequence runs with its count until it returns nothing', () => {
  // The sequence waits half a second between its three runs.
  const runs: number[] = []
  frame(0)
  const timer = sequence((run) => {
    runs.push(run)
    return run < 3 ? 0.5 : undefined
  })

  // The first run comes on the next frame, the rest after each wait.
  frame(0)
  frame(0.4)
  expect(runs).toEqual([1])
  frame(0.1)
  frame(0.5)
  expect(runs).toEqual([1, 2, 3])
  expect(timer.finished).toBe(true)
})

test('a callback that throws ends only its own timer', () => {
  // One repeat throws and another counts.
  let survivor = 0
  frame(0)
  const broken = every(1, () => {
    throw new Error('boom')
  })
  every(1, () => survivor++)

  // Both come due twice; only the broken one stops, and it logs.
  frame(1)
  frame(1)
  expect(broken.finished).toBe(true)
  expect(survivor).toBe(2)
  expect(logged.length).toBeGreaterThan(0)
})

test('a timer made in a callback waits for the next frame', () => {
  // The outer timer makes another that is due at once.
  const runs: string[] = []
  frame(0)
  after(0, () => {
    runs.push('outer')
    after(0, () => runs.push('inner'))
  })

  // The inner timer runs a frame after the outer one.
  frame(0)
  expect(runs).toEqual(['outer'])
  frame(0)
  expect(runs).toEqual(['outer', 'inner'])
})
