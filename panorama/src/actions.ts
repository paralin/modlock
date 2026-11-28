// A game action's press belongs to the player's game: the renderer reports
// each phase of it to the mods whose roots follow it, and ends the press
// itself when the player's input moves to a stock overlay.

import { ActionPhase, GameAction } from '../../src/wasm/ui/ui.pb.js'
import type { MenuHost } from './menu.js'

/** Send runs the console command that reports one phase to mod. */
export type Send = (mod: string, action: GameAction, phase: ActionPhase) => void

/** gameActionCommand is the console command that reports phase of action to mod. */
export function gameActionCommand(
  mod: string,
  action: GameAction,
  phase: ActionPhase,
): string {
  return `modlockgame ${mod} ${action} ${phase}`
}

/**
 * Press is the state of one game action: up, held by the mods it was
 * reported to, or down after a cancellation, when the release reports
 * nothing.
 */
type Press =
  | { state: 'up' }
  | { state: 'held'; mods: Set<string> }
  | { state: 'cancelled' }

/** GameActions reports the phases of the game's bound actions. */
export class GameActions {
  private following = new Map<GameAction, Set<string>>()
  private readonly presses = new Map<GameAction, Press>()
  private watching = false

  constructor(
    private readonly send: Send,
    private readonly host: Pick<MenuHost, 'allowed' | 'stock'>,
    private readonly schedule?: (seconds: number, callback: () => void) => void,
  ) {}

  /** follow sets which mods follow each action, as the screen's roots ask. */
  follow(following: Map<GameAction, Set<string>>): void {
    // A mod that stops following during a press hears nothing more of it.
    this.following = following
    for (const [action, press] of this.presses) {
      if (press.state !== 'held') continue
      const mods = following.get(action)
      for (const mod of press.mods) if (!mods?.has(mod)) press.mods.delete(mod)
    }
  }

  /** change takes the game's report that action went down or up. */
  change(action: GameAction, down: boolean): void {
    const press = this.presses.get(action) ?? { state: 'up' }
    if (down) {
      // A repeat while down, or a press a stock overlay holds, reports nothing.
      if (press.state !== 'up') return
      const mods = this.following.get(action)
      if (!mods?.size || !this.available()) {
        this.presses.set(action, { state: 'cancelled' })
        return
      }
      this.presses.set(action, { state: 'held', mods: new Set(mods) })
      for (const mod of mods) this.send(mod, action, ActionPhase.PRESSED)
      this.watch()
      return
    }
    this.presses.set(action, { state: 'up' })
    if (press.state === 'held') {
      for (const mod of press.mods) this.send(mod, action, ActionPhase.RELEASED)
    }
  }

  /** cancel ends every held press, as the player's input leaves the HUD. */
  cancel(): void {
    for (const [action, press] of this.presses) {
      if (press.state !== 'held') continue
      this.presses.set(action, { state: 'cancelled' })
      for (const mod of press.mods)
        this.send(mod, action, ActionPhase.CANCELLED)
    }
  }

  private available(): boolean {
    return this.host.allowed() && !this.host.stock()
  }

  private watch(): void {
    // Read the stock overlays only while a press is held.
    if (this.watching || !this.schedule) return
    this.watching = true
    this.schedule(0.05, () => {
      this.watching = false
      const held = [...this.presses.values()].some((p) => p.state === 'held')
      if (!held) return
      if (this.available()) this.watch()
      else this.cancel()
    })
  }
}

/**
 * ClassAction reports a game action that the game shows only as a class on
 * its HUD, such as Extra Info's gDetailView. Panorama has no event for a
 * class change, so while some mod follows the action it reads the class
 * every twentieth of a second and hands each change to the game actions.
 */
export class ClassAction {
  private following = false
  private down = false
  private scheduled = false

  constructor(
    private readonly action: GameAction,
    private readonly read: () => boolean,
    private readonly actions: Pick<GameActions, 'change'>,
    private readonly schedule: (seconds: number, callback: () => void) => void,
  ) {}

  /** follow starts or stops reading as the screen's roots follow the action. */
  follow(following: Map<GameAction, Set<string>>): void {
    // Read at once when a mod starts to follow, then on each tick.
    this.following = (following.get(this.action)?.size ?? 0) > 0
    if (this.following) {
      this.sample()
      this.tick()
      return
    }

    // With no mod following, a held press ends unreported.
    if (this.down) {
      this.down = false
      this.actions.change(this.action, false)
    }
  }

  private tick(): void {
    if (this.scheduled) return
    this.scheduled = true
    this.schedule(0.05, () => {
      this.scheduled = false
      if (!this.following) return
      this.sample()
      this.tick()
    })
  }

  private sample(): void {
    const down = this.read()
    if (down === this.down) return
    this.down = down
    this.actions.change(this.action, down)
  }
}
