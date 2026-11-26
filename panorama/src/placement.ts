// A mod whose root follows placement hears where the player's screen puts its
// interface: the screen's size, the HUD's frame, and the bounds of each node
// it measures, so a world preview or an overlay lines up with them at any
// aspect ratio. Panorama says nothing as it lays a panel out, so
// PlacementFollow reads the layout after each draw and four times a second,
// and only while some mod follows, and reports only what changed. The host
// keeps the boxes reported before, so each report carries the screen and the
// HUD and the boxes that changed since the last.

import type { Box, Measured } from './draw.js'

/**
 * maxPlacementLine is the most characters of one report, so its console
 * command stays inside the engine's line; a larger change goes in several.
 */
export const maxPlacementLine = 480

/** placementTick is how often, in seconds, a follower reads the layout again. */
export const placementTick = 0.25

/**
 * placementCommands are the console commands that report a change of
 * placement to mod: the screen's width and height and the HUD's box, then
 * each changed box with its name, all in whole layout units. A change too
 * large for one line goes in several, each with the screen and the HUD.
 */
export function placementCommands(
  mod: string,
  measured: Measured,
  changed: Iterable<[string, Box]>,
): string[] {
  // Every command starts with the screen and the HUD's frame.
  const head = `modlockplacement ${mod} ${measured.screen.join(' ')} ${measured.hud.join(' ')}`
  const lines: string[] = []
  let line = head
  for (const [name, box] of changed) {
    // A box that does not fit the line starts the next command.
    const entry = ` ${name} ${box.join(' ')}`
    if (line !== head && line.length + entry.length > maxPlacementLine) {
      lines.push(line)
      line = head
    }
    line += entry
  }
  lines.push(line)
  return lines
}

/** Send runs one console command. */
export type Send = (line: string) => void

/**
 * PlacementFollow reports each following mod's placement: all of it when the
 * mod starts to follow, then each change.
 */
export class PlacementFollow {
  /** mods holds the generation of each mod whose root follows placement. */
  private mods = new Map<string, bigint>()
  /**
   * sent holds, for each following mod, the placement it last reported and
   * the generation of the tree it reported it for.
   */
  private readonly sent = new Map<
    string,
    { generation: bigint; measured: Measured }
  >()
  private ticking = false

  constructor(
    private readonly measure: () => Map<string, Measured>,
    private readonly send: Send,
    private readonly schedule: (seconds: number, callback: () => void) => void,
  ) {}

  /**
   * follow sets the mods whose roots follow placement, each with its tree's
   * generation, as a draw finishes, and reads the placement now; an empty
   * map stops the reading.
   */
  follow(mods: Map<string, bigint>): void {
    this.mods = mods
    this.report()
    this.tick()
  }

  private report(): void {
    // A mod that stopped following, or cleared its tree, which the host
    // forgets with it, hears all of it again.
    for (const [mod, { generation }] of this.sent) {
      if (this.mods.get(mod) !== generation) this.sent.delete(mod)
    }
    if (this.mods.size === 0) return
    const now = this.measure()

    // Send each mod the boxes that changed, or the screen and HUD alone.
    for (const [mod, measured] of now) {
      const generation = this.mods.get(mod)
      if (generation === undefined) continue
      const before = this.sent.get(mod)?.measured
      const changed = [...measured.boxes].filter(
        ([name, box]) => !same(before?.boxes.get(name), box),
      )
      if (
        before &&
        changed.length === 0 &&
        same(before.screen, measured.screen) &&
        same(before.hud, measured.hud)
      )
        continue
      for (const line of placementCommands(mod, measured, changed)) {
        this.send(line)
      }
      this.sent.set(mod, { generation, measured })
    }
  }

  private tick(): void {
    // Read again shortly while some mod follows; stop once none does.
    if (this.ticking || this.mods.size === 0) return
    this.ticking = true
    this.schedule(placementTick, () => {
      this.ticking = false
      this.report()
      this.tick()
    })
  }
}

/** same reports whether two boxes or sizes hold the same numbers. */
function same(
  a: readonly number[] | undefined,
  b: readonly number[] | undefined,
): boolean {
  return !!a && !!b && a.length === b.length && a.every((v, i) => v === b[i])
}
