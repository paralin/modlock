// The game's HUD panels persist from game to game, so a part a screen hides
// stays hidden in every later game, ranked matches included, unless something
// shows it again. The stock guard is the one writer of those panels. It
// reconciles on a level rather than on events: every tick it hides what the
// live screen claims while it is allowed to, and shows again everything its
// marks record otherwise, so a lost event, a reloaded script or a server that
// just goes away is undone by the next tick. Its states and the rules they
// keep are in ../StockGuard.tla.

import { type AbilityRow, HudPart } from '../../src/wasm/ui/ui.pb.js'
import { children } from './abilities.js'
import { type AbilityHost, AbilityHud } from './ability-hud.js'

export { BAR_GATE, BAR_MARK, SAME_CLICK_SECONDS } from './ability-hud.js'

/** GuardPanel is the part of a game panel the guard uses. */
export interface GuardPanel {
  readonly id: string
  readonly paneltype: string
  visible: boolean
  hittest: boolean
  readonly style: Record<string, string | null>
  IsValid(): boolean
  FindChildTraverse(id: string): GuardPanel | null
  FindChildrenWithClassTraverse(name: string): GuardPanel[]
  GetChildCount(): number
  GetChild(index: number): GuardPanel | null
  BHasClass(name: string): boolean
  GetParent(): GuardPanel | null
  GetAttributeString(name: string, fallback: string): string
  SetAttributeString(name: string, value: string): void
  SetImage(path: string): void
  SetPanelEvent(event: string, handler: () => void): void
  ClearPanelEvent(event: string): void
  DeleteAsync(seconds: number): void
}

/** GuardHost is the guard's clock, and what its ability bar and display reach. */
export interface GuardHost extends AbilityHost {
  /** Schedule runs callback once after seconds, as $.Schedule does. */
  Schedule(seconds: number, callback: () => void): void
}

/**
 * HIDDEN_MARK names the attribute on each panel the guard hid: "shown" when
 * the panel showed before, "unseen" when it did not. The mark outlives the
 * script, so a guard that starts again shows what an earlier one hid.
 */
export const HIDDEN_MARK = 'modlock_hidden'

/** TICK_SECONDS is the tick while anything is hidden. */
export const TICK_SECONDS = 0.1

/**
 * WALK_SECONDS is how long the whole HUD's panels are kept between walks: a
 * walk reaches thousands of panels, too many for each tick, so the tick
 * checks the kept ones with IsValid and walks again when one is gone or the
 * kept ones are this old.
 */
export const WALK_SECONDS = 1

/**
 * KEPT_ID names the panels HUD_PART_ALL leaves: the mods' interfaces under
 * Modlock's and Hyperline's panels, and the game's tooltips.
 */
const KEPT_ID = /^(Modlock|Hyperline|TooltipManager$)/

/** WALK_LIMIT bounds the panels a walk of the whole HUD looks at. */
const WALK_LIMIT = 20000

/**
 * LEASE_SECONDS is how long the last title keeps the guard allowed: the relay
 * sends a status title every 5 s while the server lists the player, so a
 * longer silence means the stream or the server is gone.
 */
export const LEASE_SECONDS = 30

/**
 * CONNECTED_TO_HIDEOUT is the class the game keeps on its HUD while the
 * client's server is the hideout, which it adds and drops with no event.
 */
export const CONNECTED_TO_HIDEOUT = 'connectedToHideout'

/** Scope is what a finder knows beyond the game's HUD. */
export interface Scope {
  /** kept are panels HUD_PART_ALL leaves, beside those KEPT_ID names. */
  readonly kept: readonly GuardPanel[]
  /**
   * restoring is a restore: it finds every panel a mark may stand on, not
   * only those to hide now.
   */
  readonly restoring: boolean
}

/** Finder returns a part's panels under the game's HUD. */
export type Finder = (hud: GuardPanel, scope: Scope) => GuardPanel[]

/**
 * finders returns each part's panels under the game's HUD, as the game's
 * hud.xml, its level-up layout and the Broker's popup name them.
 */
export const finders: Partial<Record<HudPart, Finder>> = {
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
  // The bar's chat stays: players see all chat messages and typing there.
  [HudPart.TOP_BAR]: (hud) => {
    const bar = hud.FindChildTraverse('TopBar')
    const parts: GuardPanel[] = []
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
  [HudPart.STATS]: (hud) => ownPanel(hud, 'hudPlayerStats'),
  // The corner mark sits outside the game's HUD, under the interface's root.
  [HudPart.STATUS]: (hud) => ownPanel(top(hud), 'DefaultStatus'),
  // The game fades the picker out by a class once a team is joined, so
  // showing it again is safe.
  [HudPart.HERO_PICKER]: (hud) => ownPanel(hud, 'join_team'),
  // The Broker's popup is under the interface's root, made each time it
  // opens; it is looked for only while the player stands at his booth.
  [HudPart.BROKER_COUNTDOWN]: (hud, { restoring }) => {
    // Look only at the booth, unless restoring, then find the countdown.
    const zone = hud
      .FindChildTraverse('context_action_container')
      ?.BHasClass('in_corrupted_shop_zone')
    if (!zone && !restoring) return []
    const popup = top(hud).FindChildTraverse('ModsCorruptContainer')
    const countdown = popup?.FindChildTraverse('Countdown')
    return countdown ? [countdown] : []
  },
  [HudPart.ALL]: (hud, scope) => everything(hud, scope),
}

/**
 * everything returns the game's HUD panels beside the way down to the
 * panels that stay: those KEPT_ID names and kept. Hiding them hides the
 * whole HUD but those. A restore also takes each marked panel the walk
 * passes, so a panel now on the way, or one an earlier guard hid, comes
 * back too.
 */
function everything(hud: GuardPanel, { kept, restoring }: Scope): GuardPanel[] {
  // Find the panels that stay, level by level, not looking inside them.
  const stays = (panel: GuardPanel) =>
    KEPT_ID.test(panel.id) || kept.includes(panel)
  const found = [...kept]
  const marked: GuardPanel[] = []
  const queue = [hud]
  for (let next = 0; next < queue.length && next < WALK_LIMIT; next++) {
    for (const child of children(queue[next]!)) {
      if (stays(child)) found.push(child)
      else queue.push(child)
      if (restoring && child.GetAttributeString(HIDDEN_MARK, '') !== '') {
        marked.push(child)
      }
    }
  }

  // Mark the way down to each, then take every panel beside it.
  const way = new Set<GuardPanel>()
  for (const panel of found) {
    for (
      let at: GuardPanel | null = panel;
      at && at !== hud;
      at = at.GetParent()
    )
      way.add(at)
  }
  const parts: GuardPanel[] = []
  const walk = (parent: GuardPanel) => {
    for (const child of children(parent)) {
      if (stays(child)) continue
      if (way.has(child)) walk(child)
      else parts.push(child)
    }
  }
  walk(hud)
  return [...parts, ...marked]
}

/** top returns the root of the interface panel sits in. */
function top(panel: GuardPanel): GuardPanel {
  let root = panel
  for (let up = root.GetParent(); up; up = up.GetParent()) root = up
  return root
}

/** ownPanel finds the one panel with id under the game's HUD. */
function ownPanel(hud: GuardPanel, id: string): GuardPanel[] {
  const panel = hud.FindChildTraverse(id)
  return panel ? [panel] : []
}

/**
 * hidePanel hides one panel by its visible, marking whether it showed. A
 * panel the game shows again meanwhile is hidden again and marked shown.
 * Visible, unlike an inline visibility, comes back at once when set again.
 */
export function hidePanel(panel: GuardPanel): void {
  if (panel.visible) {
    panel.SetAttributeString(HIDDEN_MARK, 'shown')
    panel.visible = false
  } else if (panel.GetAttributeString(HIDDEN_MARK, '') === '') {
    panel.SetAttributeString(HIDDEN_MARK, 'unseen')
  }
}

/**
 * restorePanel undoes what a guard's mark on panel records. A panel that
 * showed comes back; one that did not is left to the game, since setting
 * visible overrides its style sheets: gameplay_hud stays collapsed until a
 * player joins a team. An inline collapse from Modlock's renderer before the
 * guard goes too.
 */
export function restorePanel(panel: GuardPanel): void {
  const mark = panel.GetAttributeString(HIDDEN_MARK, '')
  if (mark !== '') {
    panel.SetAttributeString(HIDDEN_MARK, '')
    if (mark === 'shown') panel.visible = true
  }
  if (panel.style.visibility === 'collapse') panel.style.visibility = null
}

/**
 * ALL_MARK names the attribute on the game's HUD while a guard hides the
 * whole HUD, so a restore walks it only then: the walk reaches thousands of
 * panels, and a restore runs on every tick that hides nothing.
 */
export const ALL_MARK = 'modlock_hidden_all'

/**
 * HudParts hides and restores the game's HUD parts under one HUD, keeping the
 * whole HUD's panels between walks. Modlock's StockGuard and Hyperline's
 * resident each hold one, so both find, mark and give back the same panels.
 */
export class HudParts {
  /** walked holds the whole HUD's panels from the last walk, and when. */
  private walked: { panels: GuardPanel[]; at: number } | null = null

  /**
   * HudParts writes on hud's panels; now is the clock in seconds; HUD_PART_ALL
   * leaves kept, such as the layout that runs the guard.
   */
  constructor(
    private readonly hud: GuardPanel,
    private readonly now: () => number,
    private readonly kept: readonly GuardPanel[] = [],
  ) {}

  /** hide hides part's panels, the ones the game built since included. */
  hide(part: HudPart): void {
    if (
      part === HudPart.ALL &&
      this.hud.GetAttributeString(ALL_MARK, '') === ''
    )
      this.hud.SetAttributeString(ALL_MARK, '1')
    for (const panel of this.find(part)) hidePanel(panel)
  }

  /**
   * restore gives back every panel a mark on part's panels records, an
   * earlier script's included.
   */
  restore(part: HudPart): void {
    if (part === HudPart.ALL) {
      // Walk the whole HUD only while a guard hides it, then drop the mark.
      this.walked = null
      if (this.hud.GetAttributeString(ALL_MARK, '') === '') return
      for (const panel of everything(this.hud, this.scope(true)))
        restorePanel(panel)
      this.hud.SetAttributeString(ALL_MARK, '')
      return
    }
    for (const panel of finders[part]?.(this.hud, this.scope(true)) ?? [])
      restorePanel(panel)
  }

  /** restoreAll gives back every part's panels. */
  restoreAll(): void {
    for (const part of Object.keys(finders)) this.restore(Number(part))
  }

  /** scope is what a finder knows beyond the HUD. */
  private scope(restoring: boolean): Scope {
    return { kept: this.kept, restoring }
  }

  /**
   * find returns part's panels to hide. The whole HUD's are kept from the
   * last walk while they are valid and newer than WALK_SECONDS; a walk that
   * no longer finds a panel the last one did, which now holds a panel that
   * stays, gives it back.
   */
  private find(part: HudPart): GuardPanel[] {
    // Find every other part afresh.
    if (part !== HudPart.ALL)
      return finders[part]?.(this.hud, this.scope(false)) ?? []

    // Keep the last walk while it is new and whole.
    const now = this.now()
    const last = this.walked
    if (
      last &&
      now - last.at < WALK_SECONDS &&
      last.panels.every((panel) => panel.IsValid())
    ) {
      return last.panels
    }

    // Walk again, giving back what the walk no longer hides.
    const panels = everything(this.hud, this.scope(false))
    const found = new Set(panels)
    for (const panel of last?.panels ?? []) {
      if (!found.has(panel) && panel.IsValid()) restorePanel(panel)
    }
    this.walked = { panels, at: now }
    return panels
  }
}

/** StockGuard hides the game's HUD parts the live screen claims. */
export class StockGuard {
  /** claims are the parts the last screen's roots hide. */
  private claims = new Set<HudPart>()
  /** applied are the parts the last tick hid. */
  private applied = new Set<HudPart>()
  /** titleAt is when the last title arrived, or -Infinity before any. */
  private titleAt = -Infinity
  /** left records a disconnect or shutdown since the last screen. */
  private left = false
  /** ticking records that a tick is scheduled. */
  private ticking = false
  /** parts finds and writes the HUD's parts. */
  private readonly parts: HudParts
  /** abilities listens on the ability bar and draws the ability display. */
  private readonly abilities: AbilityHud

  /**
   * The guard writes on hud's panels, on host's clock; HUD_PART_ALL leaves
   * kept, such as the layout that runs the guard.
   */
  constructor(
    private readonly hud: GuardPanel,
    private readonly host: GuardHost,
    kept: readonly GuardPanel[] = [],
  ) {
    // A guard before this one may have left its marks; nothing is allowed
    // yet, so they all go.
    this.parts = new HudParts(hud, () => host.now(), kept)
    this.parts.restoreAll()

    // Handlers an earlier guard left on the bar and Tab act no more, and
    // what it drew there goes too.
    this.abilities = new AbilityHud(hud, host, WALK_SECONDS, () =>
      this.allowed(),
    )
  }

  /**
   * screen takes a screen's claims, the mods whose roots ask for clicks on
   * the ability bar, and the rows their roots show on Tab and the bar; null
   * is a relay that ended.
   */
  screen(
    parts: Iterable<HudPart> | null,
    presses: readonly string[] = [],
    rows: readonly AbilityRow[] = [],
  ): void {
    // Take the claims and renew the lease, then reconcile.
    this.claims = new Set([...(parts ?? [])].filter((part) => finders[part]))
    this.abilities.take(presses, rows)
    this.titleAt = this.host.now()
    this.left = false
    this.tick()
  }

  /** alive takes the relay's status title, which keeps the lease. */
  alive(): void {
    this.titleAt = this.host.now()
    this.tick()
  }

  /**
   * leave takes the game's word that the player is leaving: a disconnect or
   * shutdown. Nothing is allowed until the next screen.
   */
  leave(): void {
    this.left = true
    this.tick()
  }

  /**
   * allowed reports whether the screen may hold the game's HUD: a title
   * within the lease, no leave since the last screen, and no hideout.
   */
  allowed(): boolean {
    return (
      this.host.now() - this.titleAt < LEASE_SECONDS &&
      !this.left &&
      !this.hud.BHasClass(CONNECTED_TO_HIDEOUT)
    )
  }

  /**
   * tick makes the HUD match the claims: it shows each part no longer
   * wanted, hides each wanted part's panels, the ones the game built since
   * included, and ticks again while anything is hidden. Allowed comes back
   * only with a title, which ticks, so nothing needs a tick while nothing
   * is hidden.
   */
  private tick(): void {
    // Show what is no longer wanted, then hide what is.
    const want = this.allowed() ? this.claims : new Set<HudPart>()
    for (const part of this.applied) {
      if (!want.has(part)) this.parts.restore(part)
    }
    for (const part of want) this.parts.hide(part)
    this.applied = new Set(want)

    // Listen on the bar and draw the display's rows, or give them back.
    const abilities = this.abilities.apply()

    // Tick while a part is hidden, to hide it again or give it back, and
    // while a mod asks for the bar or rows are drawn, to reach panels the
    // game rebuilt.
    if ((this.applied.size !== 0 || abilities) && !this.ticking) {
      this.ticking = true
      this.host.Schedule(TICK_SECONDS, () => {
        this.ticking = false
        this.tick()
      })
    }
  }
}
