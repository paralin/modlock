// A menu's open state belongs to the renderer, not to the tree that describes
// its content. In particular, an ordinary redraw cannot undo a dismissal.

import { type Menu, MenuMode } from '../../src/wasm/ui/ui.pb.js'
import type { Panel, Panorama } from './draw.js'

/** MenuHost reads the stock HUD and grants the menu its share of the cursor. */
export interface MenuHost {
  allowed(): boolean
  stock(): 'escape' | 'other' | null
  capture(open: boolean): void
  resume(): void
}

/** Surface is one mounted menu with its content and optional opener. */
export interface Surface {
  readonly panel: Panel
  readonly content: Panel
  readonly opener: Panel
  readonly label: Panel
  mode: MenuMode
  request: number
  epoch: number
  dismiss?: () => void
}

/** Menus gives at most one live surface the local cursor and keyboard. */
export class Menus {
  private surfaces = new Map<string, Surface>()
  private next = new Map<string, Surface>()
  private active?: Surface
  private held = false
  private ticking = false
  private requested?: Surface

  // Keep recent open requests after their panel or screen disappears. A
  // repeated snapshot cannot bring one back, even after a bridge interruption.
  private readonly requests = new Map<string, number>()

  constructor(
    private readonly api: Panorama,
    private readonly host?: MenuHost,
  ) {}

  /** mount keeps a surface through redraws while its panel and mode stay. */
  mount(key: string, panel: Panel, menu: Menu, flow: string): Surface {
    // Keep the panel and its local state until the drawing replaces it.
    const mode = menu.mode ?? MenuMode.MANUAL
    const old = this.surfaces.get(key)
    let surface = old
    if (old?.panel !== panel) {
      const content = this.api.CreatePanel(
        'Panel',
        panel,
        'ModlockMenuContent',
        {
          acceptsfocus: 'true',
        },
      )
      content.hittest = false
      content.style.width = '100%'
      content.style.height = 'fit-children'
      const opener = this.api.CreatePanel('Button', panel, 'ModlockMenuOpen')
      const label = this.api.CreatePanel(
        'Label',
        opener,
        'ModlockMenuOpenLabel',
      )
      label.hittest = false
      Object.assign(opener.style, {
        padding: '8px 16px',
        backgroundColor: '#39342d',
      })
      Object.assign(label.style, { color: '#ffefd7', fontSize: '18px' })
      surface = {
        panel,
        content,
        opener,
        label,
        mode,
        request: this.requests.get(key) ?? 0,
        epoch: 0,
      }
      const mounted = surface
      opener.SetPanelEvent('onactivate', () => {
        if (this.surfaces.get(key) === mounted && opener.visible) {
          this.open(mounted)
        }
      })
      content.SetPanelEvent('oncancel', () => this.cancel(mounted))
    }

    // A mode change ends capture without treating a held key as a new press.
    const current = surface!
    if (current.mode !== mode) {
      this.cancel(current)
      current.mode = mode
    }
    current.content.style.flowChildren = flow
    current.label.text = menu.openLabel ?? ''
    this.next.set(key, current)

    // Consume open requests even if the stock HUD will decline this one.
    if ((menu.openRequest ?? 0) > current.request) {
      current.request = menu.openRequest!
      this.requests.delete(key)
      this.requests.set(key, current.request)
      if (this.requests.size > 512)
        this.requests.delete(this.requests.keys().next().value!)
      // Replacing a panel is a close, even if a redraw delivered a request.
      if (!old || old === current) this.requested = current
    }
    this.visibility(current)
    return current
  }

  /** finish drops surfaces not reached by the current drawing. */
  finish(): void {
    // Reconcile ownership before acting on an explicit open request.
    if (this.active && ![...this.next.values()].includes(this.active))
      this.close()
    this.surfaces = this.next
    this.next = new Map()
    const requested = this.requested
    this.requested = undefined
    if (requested) this.open(requested)
    this.follow()
  }

  /** scoreboard takes the game's bound action, not a physical key. */
  scoreboard(pressed: boolean): void {
    // A key still held after dismissal cannot reopen the surface.
    if (pressed === this.held) return
    this.held = pressed
    if (!pressed) {
      if (this.active?.mode === MenuMode.HOLD) this.close()
      return
    }
    if (!this.available()) return
    if (this.active?.mode === MenuMode.TOGGLE) {
      this.close()
      return
    }
    if (this.active) return
    for (const surface of this.surfaces.values()) {
      if (surface.mode !== MenuMode.MANUAL) {
        this.open(surface)
        break
      }
    }
  }

  /** accepts reports whether a descendant can send input now. */
  accepts(surface: Surface): boolean {
    return this.active === surface && this.available()
  }

  /** cancel dismisses only the surface whose handler fired. */
  cancel(surface: Surface): boolean {
    if (this.active !== surface) return false
    const handled = this.available()
    this.close()
    return handled
  }

  /** close hides the surface and returns only the focus the menu owned. */
  close(): void {
    // Retire all callbacks from this open before handing focus back.
    const active = this.active
    if (!active) return
    this.active = undefined
    active.epoch++
    active.dismiss?.()
    if (
      active.content.IsValid() &&
      (active.content.BHasKeyFocus?.() ||
        active.content.BHasDescendantKeyFocus?.())
    ) {
      this.api.DispatchEvent('DropInputFocus', active.content)
    }
    this.visibility(active)
    this.host?.capture(false)
  }

  private available(): boolean {
    return (this.host?.allowed() ?? true) && !this.host?.stock()
  }

  private open(surface: Surface): void {
    // Only a live surface with no stock overlay can take capture.
    if (
      !surface.panel.IsValid() ||
      !this.available() ||
      this.active === surface
    )
      return
    this.close()
    this.active = surface
    this.visibility(surface)
    this.host?.capture(true)
    surface.content.SetFocus?.()
    this.follow()
  }

  private visibility(surface: Surface): void {
    // The opener stays available while the contents are hidden.
    if (!surface.panel.IsValid()) return
    const open = this.active === surface
    surface.content.visible = open
    surface.opener.visible = !open && surface.label.text !== ''
    surface.panel.visible = open || surface.opener.visible
  }

  private follow(): void {
    if (!this.active || this.ticking || !this.api.Schedule || !this.host) return
    this.ticking = true
    this.api.Schedule(0.05, () => {
      // Inspect the live stock panels only while the menu holds capture.
      this.ticking = false
      if (!this.active) return
      const stock = this.host!.stock()
      const live = this.host!.allowed() && this.active.panel.IsValid()
      if (!live || stock) {
        this.close()
        // Escape may go straight to the HUD when no descendant has focus.
        if (live && stock === 'escape') this.host!.resume()
      } else {
        this.follow()
      }
    })
  }
}
