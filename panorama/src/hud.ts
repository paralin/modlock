// Hud hides parts of the game's own HUD while a mod's root lists them, and
// shows them again once no root does. The game builds some of those panels
// later, such as a notice for each level up, and rebuilds the HUD on its own,
// and Panorama has no event for a new panel, so while any part is hidden the
// hud looks for its panels again every sweepSeconds.

import { HudPart, type Screen } from '../../src/wasm/ui/ui.pb.js'

/** HudPanel is the part of a game panel the hud uses. */
export interface HudPanel {
  readonly style: Record<string, string | null>
  FindChildTraverse(id: string): HudPanel | null
  FindChildrenWithClassTraverse(name: string): HudPanel[]
  GetChildCount(): number
  GetChild(index: number): HudPanel | null
  BHasClass(name: string): boolean
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
}

/** Hud collapses the parts of the game's HUD that the screen's roots list. */
export class Hud {
  /** hidden holds the parts some root lists. */
  private hidden = new Set<HudPart>()

  /** sweeping records that a sweep is scheduled. */
  private sweeping = false

  constructor(
    private readonly hud: HudPanel,
    private readonly schedule: Schedule,
  ) {}

  /** draw hides the parts screen's roots list and shows the rest again. */
  draw(screen: Screen | null): void {
    // Gather the parts every mod's root lists.
    const hidden = new Set<HudPart>()
    for (const tree of screen?.trees ?? []) {
      const root = tree.nodes?.find((node) => (node.id ?? '') === '')
      for (const part of root?.hideHud ?? []) {
        if (finders[part]) {
          hidden.add(part)
        }
      }
    }

    // Show the parts no root lists any more, then hide the rest.
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
