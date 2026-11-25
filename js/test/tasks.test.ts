import { expect, test } from 'bun:test'

import { Call, FrameEvent, Reply } from '../../src/wasm/wasm.pb.js'
import { bind, deliver } from '../src/host.js'
import { task } from '../src/index.js'

// The library answers every host call it makes with an empty reply.
bind(() => Reply.toBinary({}))

/** clock is the game time of the next frame. */
let clock = 0

/**
 * frame delivers a frame with budget microseconds, then lets the resumed
 * tasks run, as the runtime drains its jobs before the event returns.
 */
async function frame(budget: number): Promise<void> {
  clock += 1 / 64
  deliver(
    Call.toBinary({
      method: 'Frame',
      request: FrameEvent.toBinary({
        timeSeconds: clock,
        budgetMicroseconds: budget,
      }),
    }),
  )
  await new Promise((settle) => setTimeout(settle, 0))
}

/** spin holds the thread for ms milliseconds. */
function spin(ms: number): void {
  const until = performance.now() + ms
  while (performance.now() < until) {
    // Spend the time.
  }
}

test('without a budget a task runs to its end in the frame it starts', async () => {
  // Pace between ten steps; with no budget pace never waits.
  let steps = 0
  const work = task(async (t) => {
    for (let i = 0; i < 10; i++) {
      steps++
      await t.pace()
    }
  })
  await frame(0)
  expect(steps).toBe(10)
  expect(work.finished).toBe(true)
})

test('every task takes a step in each frame however costly the others', async () => {
  // A 1 ms step against a 100 us budget: the first task is late at once.
  const steps = [0, 0]
  for (const index of [0, 1]) {
    task(async (t) => {
      for (;;) {
        spin(1)
        steps[index]!++
        await t.pace()
      }
    })
  }
  await frame(100)
  await frame(100)
  await frame(100)
  expect(steps).toEqual([3, 3])
})

test('a cancelled task runs no further step and unwinds', async () => {
  let steps = 0
  let unwound = false
  const work = task(async (t) => {
    try {
      for (;;) {
        steps++
        await t.frame()
      }
    } finally {
      unwound = true
    }
  })
  await frame(100)
  work.cancel()
  await frame(100)
  await frame(100)
  expect(steps).toBe(1)
  expect(unwound).toBe(true)
  expect(work.finished).toBe(true)
})
