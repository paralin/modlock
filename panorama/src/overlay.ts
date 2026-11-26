// A stock overlay, such as chat, the shop or the escape menu, takes the
// player's input from the HUD. OverlayReader names the one that holds it now,
// for the menus and game actions that yield to it, and OverlayFollow reports
// each change to the mods whose roots follow overlays.

import { Overlay } from '../../src/wasm/ui/ui.pb.js'
import { CHAT_OPEN } from './draw.js'
import type { MenuHost } from './menu.js'

/** overlayCommand is the console command that reports overlay to mod. */
export function overlayCommand(mod: string, overlay: Overlay): string {
  return `modlockoverlay ${mod} ${overlay}`
}

/** stockOf is the overlay as the menus see it: the escape menu, another, or none. */
export function stockOf(overlay: Overlay): ReturnType<MenuHost['stock']> {
  if (overlay === Overlay.NONE) return null
  return overlay === Overlay.ESCAPE_MENU ? 'escape' : 'other'
}

/**
 * OverlayReader reads the game's HUD for the overlay that holds the player's
 * input. It keeps the chat box and the shop it found, since a follower reads
 * them every tick, and finds them again once the game replaces them.
 */
export class OverlayReader {
  private readonly found = new Map<string, Panel>()

  constructor(private readonly game: Panel) {}

  /** read names the first open overlay, in the order the Overlay enum gives. */
  read(): Overlay {
    // Chat holds the input while open or while its entry has focus.
    const chat = this.find('Chat')
    if (
      chat?.BHasClass(CHAT_OPEN) ||
      chat?.FindChildTraverse('ChatInput')?.BHasKeyFocus?.()
    )
      return Overlay.CHAT
    if (this.find('CitadelHudHeroShop')?.BHasClass('gShopOpen'))
      return Overlay.SHOP
    if (this.game.BHasClass('gEditingBuilds')) return Overlay.BUILDS
    if (this.game.BHasClass('gItemDraftOpen')) return Overlay.ITEM_DRAFT

    // A stock popup shows while its panel is not hidden.
    const manager = this.game.FindChild('PopupManager')
    for (let i = 0; i < (manager?.GetChildCount() ?? 0); i++) {
      const panel = manager!.GetChild(i)
      if (panel?.BHasClass('PopupPanel') && !panel.BHasClass('Hidden'))
        return Overlay.POPUP
    }
    if (
      this.game.BHasClass('ShowEscapeMenu') ||
      this.game.FindChild('EscapeMenu')?.BHasClass('ShowEscapeMenu')
    )
      return Overlay.ESCAPE_MENU
    return Overlay.NONE
  }

  private find(id: string): Panel | null {
    // A kept panel the game has since deleted is found again.
    const kept = this.found.get(id)
    if (kept?.IsValid()) return kept
    const panel = this.game.FindChildTraverse(id)
    if (panel) this.found.set(id, panel)
    else this.found.delete(id)
    return panel
  }
}

/** Send runs the console command that reports overlay to mod. */
export type Send = (mod: string, overlay: Overlay) => void

/**
 * OverlayFollow reports the overlay to each following mod: the current one
 * when the mod starts to follow, then each change. It reads the HUD every
 * tenth of a second, and only while some mod follows.
 */
export class OverlayFollow {
  private mods = new Set<string>()
  private last = Overlay.NONE
  private ticking = false

  constructor(
    private readonly read: () => Overlay,
    private readonly send: Send,
    private readonly schedule: (seconds: number, callback: () => void) => void,
  ) {}

  /** follow sets the mods whose roots follow overlays, as the screen asks. */
  follow(mods: Set<string>): void {
    // A new follower hears the current overlay; the others hear only a change.
    const now = this.read()
    for (const mod of mods) {
      if (!this.mods.has(mod) || now !== this.last) this.send(mod, now)
    }
    this.mods = mods
    this.last = now
    this.tick()
  }

  private tick(): void {
    if (this.ticking || !this.mods.size) return
    this.ticking = true
    this.schedule(0.1, () => {
      // Report a change to every follower, and stop once none remain.
      this.ticking = false
      if (!this.mods.size) return
      const now = this.read()
      if (now !== this.last) {
        this.last = now
        for (const mod of this.mods) this.send(mod, now)
      }
      this.tick()
    })
  }
}
