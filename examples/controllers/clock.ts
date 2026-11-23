// The clock reads the latency of the "Modlock virtual gamepad" that a client
// started with MODLOCK_VIRTUAL_CONTROLLERS=1 streams. Its left stick's x is
// (t mod 2 s) / 1 s - 1 at the moment the client read it, so the time this
// mod sees it, less that, is the delay from the client's read to the mod's
// frame when both run on one PC's clock.
import { type Controller, log } from 'modlock'

/** virtualPad is the name the client gives the virtual gamepad. */
const virtualPad = 'Modlock virtual gamepad'

/** period is the clock axis's period, in milliseconds. */
const period = 2000

/** reportEvery is how often the clock logs its percentiles, in milliseconds. */
const reportEvery = 5000

/** samples holds each slot's delays since its last report, in milliseconds. */
const samples = new Map<number, number[]>()

/** reported is when the clock last logged its percentiles. */
let reported = Date.now()

/**
 * lag returns the delay behind c in milliseconds when c is the virtual
 * gamepad, recording it for slot's report, and undefined otherwise.
 */
export function lag(slot: number, c: Controller): number | undefined {
  // Take only the virtual gamepad.
  if (c.info.name !== virtualPad || !c.info.gamepad) {
    return undefined
  }

  // Decode the moment the client read the axis, and center the delay on
  // zero, so a read the axis's rounding places a little after this mod's
  // clock counts as a small negative delay rather than as nearly a period.
  const read = (c.state.gamepad.leftX + 1) * 1000
  const delay = ((Date.now() - read + period * 1.5) % period) - period / 2

  // Keep it for the report.
  const list = samples.get(slot) ?? []
  list.push(delay)
  samples.set(slot, list)
  return delay
}

/** report logs each slot's p50, p99 and maximum delay every reportEvery. */
export function report(): void {
  // Wait for the next report.
  const now = Date.now()
  if (now - reported < reportEvery) {
    return
  }
  reported = now

  // Log each slot's percentiles, and start its next window.
  for (const [slot, list] of samples) {
    list.sort((a, b) => a - b)
    const at = (q: number) =>
      list[Math.min(list.length - 1, Math.floor(q * list.length))].toFixed(1)
    log(
      `controllers: slot ${slot} delay p50 ${at(0.5)} p99 ${at(0.99)} max ${at(1)} ms over ${list.length} frames`,
    )
  }
  samples.clear()
}
