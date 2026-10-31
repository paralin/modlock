// Hud hides parts of the game's own HUD while its host lists them, such as
// the parts mods' roots list, and shows them again once the host stops. The
// game builds some of those panels later, such as a notice for each level up,
// and rebuilds the HUD on its own, and Panorama has no event for a new panel,
// so while any part is hidden the hud looks for its panels again every
// sweepSeconds.

import { HudPart, type Screen } from '../../src/wasm/ui/ui.pb.js'

/** HudPanel is the part of a game panel the hud uses. */
export interface HudPanel {
  readonly style: Record<string, string | null>
  FindChildTraverse(id: string): HudPanel | null
  FindChildrenWithClassTraverse(name: string): HudPanel[]
  GetChildCount(): number
  GetChild(index: number): HudPanel | null
  BHasClass(name: string): boolean
  GetParent(): HudPanel | null
}

/** Schedule runs callback once after seconds, as $.Schedule does. */
export type Schedule = (seconds: number, callback: () => void) => void

/** sweepSeconds is how often the hud collapses the parts' new panels. */
const sweepSeconds = 0.1

/**
 * finders returns each part's panels under the game's HUD, as the game's
 * hud.xml and its level-up layout name them.
 */
const finders: Partial<Record<HudPart, (hud: HudPanel) => HudPanel[]>> = {
  [HudPart.LEVEL_UP]: (hud) =>
    hud
      .FindChildTraverse('CitadelHudGameAnnouncements')
      ?.FindChildrenWithClassTraverse('LevelUpBlock') ?? [],
  [HudPart.OWN_HEALTH]: (hud) => {
    const health = hud.FindChildTraverse('health_and_abilities_container')
    return ['HudShieldsContainer', 'hud_health_bars', 'HealthRegenAndTotal']
      .map((id) => health?.FindChildTraverse(id) ?? null)
      .filter((panel) => panel !== null)
  },
  [HudPart.TOP_BAR]: (hud) => {
    const bar = hud.FindChildTraverse('TopBar')
    const parts: HudPanel[] = []
    for (let index = 0; index < (bar?.GetChildCount() ?? 0); index++) {
      const part = bar!.GetChild(index)
      if (part && !part.BHasClass('ChatContainer')) {
        parts.push(part)
      }
    }
    return parts
  },
  [HudPart.AMMO]: (hud) => {
    const ammo = ['out_of_ammo_label', 'low_ammo_label'].map((id) =>
      hud.FindChildTraverse(id),
    )
    const crosshair = hud.FindChildTraverse('crosshair')
    ammo.push(
      ...['clip_status', 'ammo_panel'].map(
        (id) => crosshair?.FindChildTraverse(id) ?? null,
      ),
    )
    return ammo.filter((panel) => panel !== null)
  },
  [HudPart.SOULS]: (hud) => ownPanel(hud, 'gold_and_ap_container'),
  [HudPart.MINIMAP]: (hud) => ownPanel(hud, 'minimap_persp'),
  [HudPart.ABILITIES]: (hud) => ownPanel(hud, 'AbilitiesContainer'),
  // The corner mark sits outside the game's HUD, under the interface's root.
  [HudPart.STATUS]: (hud) => ownPanel(top(hud), 'DefaultStatus'),
}

/** top returns the root of the interface panel sits in. */
function top(panel: HudPanel): HudPanel {
  let root = panel
  for (let up = root.GetParent(); up; up = up.GetParent()) root = up
  return root
}

/** ownPanel finds the one panel with id under the game's HUD. */
function ownPanel(hud: HudPanel, id: string): HudPanel[] {
  const panel = hud.FindChildTraverse(id)
  return panel ? [panel] : []
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

/** Hud collapses the parts of the game's HUD it is told to hide. */
export class Hud {
  /** hidden holds the parts some root lists. */
  private hidden = new Set<HudPart>()

  /** sweeping records that a sweep is scheduled. */
  private sweeping = false

  constructor(
    private readonly hud: HudPanel,
    private readonly schedule: Schedule,
  ) {}

  /** draw hides parts and shows every other part it hid again. */
  draw(parts: Iterable<HudPart>): void {
    // Keep the parts the hud knows how to find.
    const hidden = new Set<HudPart>()
    for (const part of parts) {
      if (finders[part]) {
        hidden.add(part)
      }
    }

    // Show the parts no longer listed, then hide the rest.
    for (const part of this.hidden) {
      if (!hidden.has(part)) {
        for (const panel of finders[part]!(this.hud)) {
          panel.style.visibility = null
        }
      }
    }
    this.hidden = hidden
    this.sweep()
  }

  /** sweep collapses each hidden part's panels and schedules the next sweep. */
  private sweep(): void {
    for (const part of this.hidden) {
      for (const panel of finders[part]!(this.hud)) {
        if (panel.style.visibility !== 'collapse') {
          panel.style.visibility = 'collapse'
        }
      }
    }
    if (this.hidden.size !== 0 && !this.sweeping) {
      this.sweeping = true
      this.schedule(sweepSeconds, () => {
        this.sweeping = false
        this.sweep()
      })
    }
  }
}
