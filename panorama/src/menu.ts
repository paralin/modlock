// A menu's open state belongs to the renderer, not to the tree that describes
// its content. In particular, an ordinary redraw cannot undo a dismissal.
// MenuQueue.tla models the one request that waits while something blocks it.

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
  readonly mod: string
  readonly panel: Panel
  readonly content: Panel
  readonly opener: Panel
  readonly label: Panel
  mode: MenuMode
  priority: number
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
  private pending?: Surface

  // Keep recent open requests after their panel or screen disappears. A
  // repeated snapshot cannot bring one back, even after a bridge interruption.
  private readonly requests = new Map<string, number>()

  constructor(
    private readonly api: Panorama,
    private readonly host?: MenuHost,
  ) {}

  /** mount keeps a surface through redraws while its panel and mode stay. */
  mount(
    mod: string,
    id: string,
    panel: Panel,
    menu: Menu,
    flow: string,
  ): Surface {
    // Keep the panel and its local state until the drawing replaces it.
    const key = `${mod}/${id}`
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
        mod,
        panel,
        content,
        opener,
        label,
        mode,
        priority: 0,
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
    current.priority = menu.priority ?? 0
    current.content.style.flowChildren = flow
    current.label.text = menu.openLabel ?? ''
    this.next.set(key, current)

    // Consume each open request once; finish decides whether it waits.
    if ((menu.openRequest ?? 0) > current.request) {
      current.request = menu.openRequest!
      this.requests.delete(key)
      this.requests.set(key, current.request)
      if (this.requests.size > 512)
        this.requests.delete(this.requests.keys().next().value!)
      // Replacing a panel is a close, even if a redraw delivered a request.
      if (!old || old === current)
        this.requested = this.requested
          ? keep(current, this.requested)
          : current
    }
    this.visibility(current)
    return current
  }

  /** finish drops surfaces not reached by the current drawing. */
  finish(): void {
    // Reconcile ownership before acting on an explicit open request. A menu
    // the drawing left out is withdrawn, so its waiting request goes too.
    const drawn = new Set(this.next.values())
    if (this.active && !drawn.has(this.active)) this.hide()
    if (this.pending && !drawn.has(this.pending)) this.pending = undefined
    this.surfaces = this.next
    this.next = new Map()

    // This drawing's request joins the slot, and opens unless outranked.
    const requested = this.requested
    this.requested = undefined
    if (requested)
      this.pending = this.pending ? keep(requested, this.pending) : requested
    this.flush()
    this.follow()
  }

  /** scoreboard takes the game's bound action, not a physical key. */
  scoreboard(pressed: boolean): void {
    // A key still held after dismissal cannot reopen the surface.
    if (pressed === this.held) return
    this.held = pressed
    if (!pressed) {
      if (this.active?.mode === MenuMode.HOLD) this.dismiss()
      return
    }
    if (!this.available()) return
    if (this.active?.mode === MenuMode.TOGGLE) {
      this.dismiss()
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
    this.dismiss()
    return handled
  }

  /** close drops the waiting request and hides the open surface. */
  close(): void {
    this.pending = undefined
    this.hide()
  }

  private dismiss(): void {
    // A dismissal frees the cursor for the request that waited behind it.
    this.hide()
    this.flush()
    this.follow()
  }

  private hide(): void {
    // Retire all callbacks from this open, then return only the focus the
    // menu owned.
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

  private flush(): void {
    // Open the waiting request once nothing outranks it.
    const pending = this.pending
    if (!pending || !this.available()) return
    if (this.active && outranks(this.active, pending)) return
    this.pending = undefined
    this.open(pending)
  }

  private open(surface: Surface): void {
    // Only a live surface with no stock overlay can take capture.
    if (
      !surface.panel.IsValid() ||
      !this.available() ||
      this.active === surface
    )
      return
    this.hide()
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
    if (!(this.active || this.pending) || this.ticking) return
    if (!this.api.Schedule || !this.host) return
    this.ticking = true
    this.api.Schedule(0.05, () => {
      // Inspect the live stock panels only while a menu holds capture or a
      // request waits for them to close.
      this.ticking = false
      const active = this.active
      if (active) {
        const stock = this.host!.stock()
        const live = this.host!.allowed() && active.panel.IsValid()
        if (!live || stock) {
          this.hide()
          // Escape may go straight to the HUD when no descendant has focus.
          if (live && stock === 'escape') this.host!.resume()
        }
      }
      this.flush()
      this.follow()
    })
  }
}

/** outranks reports whether open menu a keeps a request for b waiting. */
function outranks(a: Surface, b: Surface): boolean {
  return a !== b && (a.mod !== b.mod || a.priority > b.priority)
}

/** keep is the one request kept when request a arrives while b waits. */
function keep(a: Surface, b: Surface): Surface {
  return a.mod === b.mod && b.priority > a.priority ? b : a
}
