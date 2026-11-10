// A mod's root may ask for clicks on the game's ability bar and name the
// abilities Tab and the bar show. Both write on the game's own panels, which
// outlive the script, so whichever guard holds the stock HUD, Modlock's or
// Hyperline's resident, holds one AbilityHud: it listens on the bar and draws
// the display only while its owner allows, and gives the display back
// otherwise. Its handlers stay on the panels after the script ends, so each
// reads the gate on the HUD and acts only while its own AbilityHud is the
// newest.

import type { AbilityRow } from '../../src/wasm/ui/ui.pb.js'
import {
  AbilityDisplay,
  barIcons,
  barPipRows,
  type DisplayHost,
  pipsOf,
} from './abilities.js'
import type { GuardPanel } from './guard.js'

/**
 * BAR_GATE names the attribute on the game's HUD that holds the token of the
 * guard whose handlers on the ability bar and Tab act. Each guard writes its
 * own as it starts, so a handler an earlier script left does nothing.
 */
export const BAR_GATE = 'modlock_bar_gate'

/** BAR_MARK names the attribute on a bar panel that holds the token of the guard listening on it. */
export const BAR_MARK = 'modlock_bar'

/** SAME_CLICK_SECONDS joins the clicks on a slot's icon and its pips: one click reaches both. */
export const SAME_CLICK_SECONDS = 0.2

/** AbilityHost is what the ability bar's handlers and the display reach. */
export interface AbilityHost extends DisplayHost {
  /** now returns the time in seconds. */
  now(): number
  /**
   * listen runs handler on each Activated event of panel, as
   * $.RegisterEventHandler does. The handler stays on the panel after the
   * script ends.
   */
  listen(panel: GuardPanel, handler: () => void): void
  /** press sends mod a press of node, as the renderer's buttons do. */
  press(mod: string, node: string): void
}

/** AbilityHud listens on the ability bar and draws the ability display. */
export class AbilityHud {
  /** presses are the mods whose roots ask for clicks on the ability bar. */
  private presses: readonly string[] = []
  /** rows are the ability display's rows the roots name. */
  private rows: readonly AbilityRow[] = []
  /** token tells this AbilityHud's handlers from an earlier script's. */
  private readonly token: string
  /** bar holds the bar's panels from the last look, and when. */
  private bar: { panels: GuardPanel[]; at: number } | null = null
  /** clickedAt holds when each slot's last click was sent. */
  private readonly clickedAt = new Map<number, number>()
  /** display draws the rows on Tab and the bar. */
  private readonly display: AbilityDisplay
  /** displayed records that the last apply drew the rows. */
  private displayed = false

  /**
   * AbilityHud writes on hud's panels through host, keeping its looks for
   * keep seconds, and acts only while allowed reports true. It takes the
   * gate from any earlier one and gives back what an earlier one drew.
   */
  constructor(
    private readonly hud: GuardPanel,
    private readonly host: AbilityHost,
    private readonly keep: number,
    private readonly allowed: () => boolean,
  ) {
    // Handlers an earlier script left on the bar and Tab act no more.
    this.token = `${host.now()}:${Math.random()}`
    hud.SetAttributeString(BAR_GATE, this.token)

    // What an earlier script drew on Tab and the bar goes too.
    this.display = new AbilityDisplay(
      hud,
      host,
      () => host.now(),
      this.keep,
      () => this.live(),
    )
    this.display.restore()
  }

  /** take holds the mods asking for the bar's clicks and the rows to show. */
  take(presses: readonly string[], rows: readonly AbilityRow[]): void {
    this.presses = presses
    this.rows = rows
  }

  /**
   * apply listens on the bar and draws the rows while allowed, and gives back
   * what it drew otherwise. It reports whether it needs another apply soon,
   * to reach panels the game rebuilt.
   */
  apply(): boolean {
    // Listen while a mod asks.
    const allowed = this.allowed()
    const listening = allowed && this.presses.length !== 0
    if (listening) this.listen()

    // Draw the rows, or give back what was drawn.
    const displaying = allowed && this.rows.length !== 0
    if (displaying) this.display.draw(this.rows)
    else if (this.displayed) this.display.restore()
    this.displayed = displaying
    return listening || displaying
  }

  /** restore gives back everything any display drew on Tab and the bar. */
  restore(): void {
    this.display.restore()
    this.displayed = false
  }

  /**
   * listen puts this AbilityHud's handler on each icon and upgrade pips of
   * the ability bar that lacks one. The bar's panels are kept between looks
   * while they are valid and newer than the display's keep.
   */
  private listen(): void {
    // Keep the last look while it is new and whole.
    const now = this.host.now()
    const last = this.bar
    if (
      last &&
      now - last.at < this.keep &&
      last.panels.every((panel) => panel.IsValid())
    )
      return

    // Find each slot's icon by its id, and its pips by where they sit.
    const panels: GuardPanel[] = []
    const rows = barPipRows(this.hud)
    for (const icon of barIcons(this.hud)) {
      const slot = Number(icon.id.slice(-1)) - 1
      for (const panel of [icon, pipsOf(icon, rows)]) {
        if (!panel) continue
        panels.push(panel)
        if (panel.GetAttributeString(BAR_MARK, '') === this.token) continue
        panel.SetAttributeString(BAR_MARK, this.token)
        this.host.listen(panel, () => this.clicked(slot))
      }
    }
    this.bar = { panels, at: now }
  }

  /**
   * live reports whether this AbilityHud's handlers on game panels act: it
   * is the one the gate names, and allowed.
   */
  private live(): boolean {
    return (
      this.hud.GetAttributeString(BAR_GATE, '') === this.token && this.allowed()
    )
  }

  /**
   * clicked sends a click on slot's icon or pips as a press to each mod that
   * asks, once per click, while this AbilityHud is the live one.
   */
  private clicked(slot: number): void {
    // Act only as the live one while allowed, once per click.
    if (!this.live()) return
    const now = this.host.now()
    if (now - (this.clickedAt.get(slot) ?? -Infinity) < SAME_CLICK_SECONDS)
      return
    this.clickedAt.set(slot, now)
    for (const mod of this.presses) this.host.press(mod, `ability:${slot}`)
  }
}
