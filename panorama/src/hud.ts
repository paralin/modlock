// The roots of a screen ask for parts of the game's HUD to be hidden and for
// the cursor; the stock guard (guard.ts) hides the parts, and Cursor frees the
// cursor.

import { type HudPart, type Screen } from '../../src/wasm/ui/ui.pb.js'

/** Dispatch dispatches a Panorama event, as $.DispatchEvent does. */
export type Dispatch = (event: string, ...values: unknown[]) => void

/**
 * Cursor tells the game to free or capture the cursor when the mods' wish
 * changes, and to free it again when a mod asks again: the player can put the
 * cursor away with the game's own keys, which this page is not told, so a
 * screen that changes anything else leaves the player's choice be.
 *
 * Capturing the cursor also drops Panorama's input focus within the game's
 * HUD, which holds the mods' panels and the stock HUD they hide. A button or
 * entry the player clicked while the cursor was free keeps the keyboard
 * otherwise, so movement and the camera stay with it until the next click.
 */
export class Cursor {
  /** free is the cursor state last sent to the game. */
  private free = false

  /** asked is the count of asks for the cursor the last screen held. */
  private asked = 0

  constructor(
    private readonly dispatch: Dispatch,
    private readonly hud: object,
  ) {}

  /** set frees the cursor or captures it for a screen that holds asks asks. */
  set(free: boolean, asks: number): void {
    // A rise in the count is an ask, which counts even when the cursor is
    // already free here.
    const again = free && asks !== this.asked
    this.asked = asks
    if (free === this.free && !again) return

    // Hand the keyboard back to the game before it takes the cursor, then
    // tell the game.
    this.free = free
    if (!free) {
      this.dispatch('DropInputFocus', this.hud)
    }
    this.dispatch('CitadelConCommand', `hud_free_cursor ${free ? 1 : 0}`)
  }
}

/** hiddenParts lists the parts of the game's HUD that screen's roots hide. */
export function hiddenParts(screen: Screen | null): HudPart[] {
  const parts: HudPart[] = []
  for (const tree of screen?.trees ?? []) {
    const root = tree.nodes?.find((node) => (node.id ?? '') === '')
    parts.push(...(root?.hideHud ?? []))
  }
  return parts
}

/** cursorFreed reports whether any of screen's roots frees the mouse cursor. */
export function cursorFreed(screen: Screen | null): boolean {
  return (screen?.trees ?? []).some(
    (tree) => tree.nodes?.find((node) => (node.id ?? '') === '')?.freeCursor,
  )
}

/**
 * cursorAsks counts the times the mods behind screen's roots asked for the
 * cursor; it rises when a mod asks again after the player put the cursor away.
 */
export function cursorAsks(screen: Screen | null): number {
  return (screen?.trees ?? []).reduce(
    (total, tree) =>
      total +
      (tree.nodes?.find((node) => (node.id ?? '') === '')?.freeCursorAsks ?? 0),
    0,
  )
}

/** abilityBarMods lists the mods whose roots in screen ask for clicks on the ability bar. */
export function abilityBarMods(screen: Screen | null): string[] {
  return (screen?.trees ?? [])
    .filter(
      (tree) =>
        tree.nodes?.find((node) => (node.id ?? '') === '')?.abilityBarPresses,
    )
    .map((tree) => tree.mod ?? '')
}
