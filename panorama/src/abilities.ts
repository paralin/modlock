// The game draws each ability icon on Tab and on the ability bar from the
// hero's own data, found by slot, so a hero holding an ability a mod gave it
// shows the hero's standard picture on Tab and no upgrades anywhere. The
// ability display draws what a root's ability_display names over those icons
// instead: on each Tab row it matches, the named picture over the game's, the
// ring and the read-only pips by the upgrades, and the game's own ability
// window on hover; on the player's own ability bar, the frame, ring, cooldown
// shade and pips by the upgrades. Every write is marked on the panel, or is
// a panel of its own, so a restore gives back what any guard drew. The
// pictures and shapes are the game's own, from its hud_ability_icon style
// sheet and layout.

import type { AbilityRow } from '../../src/wasm/ui/ui.pb.js'
import type { GuardPanel } from './guard.js'

/** BAR_DEPTH bounds how deep under hud_signature the bar's icons and pips are looked for. */
export const BAR_DEPTH = 8

/** ROW_DEPTH bounds how deep under a top bar team its player rows are looked for. */
const ROW_DEPTH = 6

/** TIERS is the number of upgrades an ability takes after its unlock. */
const TIERS = 3

/** ICONS names a Tab row's four ability icons, in slot order. */
const ICONS = [
  'AbilityButton1',
  'AbilityButton2',
  'AbilityButton3',
  'AbilityButton4',
]

/** DISPLAY_IMAGE names the picture the display lays beside an icon's own. */
export const DISPLAY_IMAGE = 'ModlockAbilityImage'

/**
 * DISPLAY_COVER names the panel the display lays over a whole Tab icon,
 * which takes the hover in place of the icon's parts: their own handlers,
 * such as the sub-ability's, stay as the game's layout set them.
 */
export const DISPLAY_COVER = 'ModlockAbilityCover'

/**
 * DRAWN_MARK names the attribute on each game panel the display styled,
 * holding what it drew, so it writes again only on a change and a restore
 * finds it.
 */
export const DRAWN_MARK = 'modlock_drawn'

/** ABILITY and PLAYER name the cover's attributes its hover reads. */
const ABILITY = 'modlock_ability'
const PLAYER = 'modlock_player'

/** PIP_ON and PIP_OFF are the game's read-only pip colors: its deadlockGreen once bought, else black. */
const PIP_ON = '#69e799'
const PIP_OFF = 'black'

/** PIP_BORDER outlines a bar pip drawn flat. */
const PIP_BORDER = '#00000099'

/** NO_WASH is a transparent color. */
const NO_WASH = '#00000000'

/** FRAMES is where the game's ability frame pictures are. */
const FRAMES = 's2r://panorama/images/hud/core/'

/** DisplayHost is what the display reaches beyond the panels. */
export interface DisplayHost {
  /**
   * create makes a panel of type under parent, as $.CreatePanel does. The
   * game's $.CreatePanel throws on an undefined properties argument, so a
   * host forwarding all four arguments passes an object in its place.
   */
  create(
    type: string,
    parent: GuardPanel,
    id: string,
    properties?: Record<string, string>,
  ): GuardPanel
  /**
   * localize reads token against panel's dialog variables, as $.Localize
   * does. The panel is always given: the game's $.Localize throws on a
   * second argument that is not a panel, so a host forwarding both
   * arguments would throw on an absent one.
   */
  localize(token: string, panel: GuardPanel): string
  /** dispatch sends a Panorama event, as $.DispatchEvent does. */
  dispatch(event: string, ...values: unknown[]): void
}

/** TabIcon is one icon of a Tab row with the parts the display writes on. */
interface TabIcon {
  readonly icon: GuardPanel
  readonly own: GuardPanel | null
  readonly ring: GuardPanel | null
  readonly pips: GuardPanel | null
}

/** TabRow is one row of Tab: its player's name and hero as shown, and its icons. */
interface TabRow {
  readonly row: GuardPanel
  readonly name: string
  readonly hero: string
  readonly icons: readonly (TabIcon | null)[]
}

/**
 * BarSlot is one slot of the ability bar: its icon, its read-only pips, and
 * its upgrade pips with each upgrade's part.
 */
interface BarSlot {
  readonly icon: GuardPanel
  readonly readOnly: GuardPanel | null
  readonly pips: GuardPanel | null
  readonly unlocks: readonly GuardPanel[]
}

/** AbilityDisplay draws the rows of the live screen's ability_display. */
export class AbilityDisplay {
  /** tab holds Tab's rows from the last look, and when. */
  private tab: { rows: TabRow[]; at: number } | null = null
  /** bar holds the bar's slots from the last look, and when. */
  private bar: { slots: BarSlot[]; at: number } | null = null

  /**
   * The display writes on hud's panels through host; now is the clock in
   * seconds; looks are kept for keep seconds; gate reports whether a hover
   * may open the game's ability window.
   */
  constructor(
    private readonly hud: GuardPanel,
    private readonly host: DisplayHost,
    private readonly now: () => number,
    private readonly keep: number,
    private readonly gate: () => boolean,
  ) {}

  /** draw makes Tab and the bar show rows. */
  draw(rows: readonly AbilityRow[]): void {
    // Match each Tab row by the player's name, then the rows left by the
    // hero's, which a bot's row shows in place of a name; each row of the
    // display serves one Tab row, so two players of a hero take one each.
    const tabs = this.tabRows()
    const heroes = rows.map((row) =>
      (row.hero ?? '') !== ''
        ? this.host.localize(`#${row.hero}`, this.hud)
        : '',
    )
    const used = new Set<number>()
    const matched = tabs.map((tab) => {
      const index = rows.findIndex(
        (row, at) =>
          !used.has(at) && (row.name ?? '') !== '' && row.name === tab.name,
      )
      if (index >= 0) used.add(index)
      return index
    })
    tabs.forEach((tab, at) => {
      if (matched[at]! >= 0 || tab.hero === '') return
      const index = heroes.findIndex(
        (hero, row) => !used.has(row) && hero === tab.hero,
      )
      if (index >= 0) used.add(index)
      matched[at] = index
    })

    // Dress each matched row's icons, and give back the rest.
    tabs.forEach((tab, at) => {
      const shown = rows[matched[at]!]
      tab.icons.forEach((icon, slot) => {
        if (!icon) return
        const ability = shown?.abilities?.[slot]
        if (shown && ability && (ability.ability ?? 0) !== 0) {
          this.dress(icon, shown.player ?? 0, ability.ability!, ability)
        } else {
          undress(icon)
        }
      })
    })

    // Draw the viewer's own row on the bar.
    const own = rows.find((row) => row.own)
    this.barSlots().forEach((slot, index) => {
      const ability = own?.abilities?.[index]
      if (ability && (ability.ability ?? 0) !== 0) {
        drawBar(slot, ability.upgrades ?? 0)
      } else {
        clearBar(slot)
      }
    })
  }

  /** restore gives Tab's icons and the bar back to the game, whichever guard drew them. */
  restore(): void {
    // Look afresh, so panels an earlier guard drew are found too.
    this.tab = null
    this.bar = null
    for (const tab of this.tabRows())
      for (const icon of tab.icons) if (icon) undress(icon)
    for (const slot of this.barSlots()) clearBar(slot)
  }

  /**
   * dress lays ability over a Tab icon of player's row: its picture over the
   * game's, its ring and read-only pips by its upgrades, and a cover whose
   * hover opens the game's window for ability as player's hero holds it.
   */
  private dress(
    icon: TabIcon,
    player: number,
    id: number,
    ability: NonNullable<AbilityRow['abilities']>[number],
  ): void {
    // Mark what the icon wears, then lay the picture beside the game's,
    // hiding the game's while one is named.
    const image = ability.image ?? ''
    const upgrades = ability.upgrades ?? 0
    mark(icon.icon, `${id} ${player} ${upgrades} ${image}`)
    const parent = icon.own?.GetParent()
    let picture = icon.icon.FindChildTraverse(DISPLAY_IMAGE)
    if (!picture && parent) {
      picture = this.host.create('Image', parent, DISPLAY_IMAGE, {
        class: 'ability_image',
        scaling: 'stretch-to-fit-preserve-aspect',
      })
      picture.hittest = false
      picture.visible = false
    }
    if (picture && picture.GetAttributeString(DRAWN_MARK, '') !== image) {
      picture.SetAttributeString(DRAWN_MARK, image)
      picture.SetImage(image)
      picture.visible = image !== ''
      if (icon.own) icon.own.style.opacity = image !== '' ? '0' : null
    }

    // Cover the icon, so the hover is the display's wherever it rests.
    let cover = icon.icon.FindChildTraverse(DISPLAY_COVER)
    if (!cover) {
      cover = this.host.create('Panel', icon.icon, DISPLAY_COVER)
      cover.style.width = '100%'
      cover.style.height = '100%'
      cover.style.ignoreParentFlow = 'true'
      const covering = cover
      cover.SetPanelEvent('onmouseover', () => {
        if (!this.gate()) return
        this.host.dispatch(
          'CitadelShowAbilityDetailsTooltip',
          icon.icon,
          Number(covering.GetAttributeString(ABILITY, '0')),
          Number(covering.GetAttributeString(PLAYER, '0')),
        )
      })
      cover.SetPanelEvent('onmouseout', () => {
        this.host.dispatch('CitadelHideAbilityDetailsTooltip', icon.icon)
      })
      // Silence the icon's own hover, which would open the game's window for
      // the hero's own ability over the cover's.
      icon.icon.SetPanelEvent('onmouseover', () => {})
    }
    cover.SetAttributeString(ABILITY, String(id))
    cover.SetAttributeString(PLAYER, String(player))

    // Draw the ring and the read-only pips by the upgrades.
    if (icon.ring && mark(icon.ring, String(upgrades))) {
      icon.ring.style.visibility = tierOf(upgrades) > 0 ? 'visible' : 'collapse'
    }
    drawReadOnlyPips(icon.pips, upgrades)
  }

  /** tabRows returns Tab's rows, kept while they are valid and newer than keep. */
  private tabRows(): TabRow[] {
    // Keep the last look while it is new and whole.
    const now = this.now()
    const last = this.tab
    if (
      last &&
      now - last.at < this.keep &&
      last.rows.every(
        (tab) =>
          tab.row.IsValid() &&
          tab.icons.every((icon) => !icon || icon.own?.IsValid()),
      )
    )
      return last.rows

    // Find the top bar's teams by their type, then each team's player rows.
    const top = this.hud.FindChildTraverse('TopBar')
    const teams = top
      ? inOrder(
          top,
          ROW_DEPTH,
          (panel) => panel.paneltype === 'CitadelHudTopBarTeam',
        )
      : []
    const rows = teams.flatMap((team) =>
      inOrder(
        team,
        ROW_DEPTH,
        (panel) => panel.paneltype === 'CitadelHudTopBarPlayer',
      ),
    )
    this.tab = {
      rows: rows.map((row) => {
        const box = row.FindChildTraverse('PlayerAbilitiesContainer')
        return {
          row,
          name: this.variable(row, 'player_name'),
          hero: this.variable(row, 'hero_name'),
          icons: ICONS.map((id) => {
            const icon = box?.FindChildTraverse(id)
            return icon
              ? {
                  icon,
                  own: icon.FindChildTraverse('ability_image'),
                  ring: firstWithClass(icon, 'ability_ring'),
                  pips: icon.FindChildTraverse('ReadOnlyPipsContainer'),
                }
              : null
          }),
        }
      }),
      at: now,
    }
    return this.tab.rows
  }

  /** variable reads a dialog variable of panel, or '' when it has none. */
  private variable(panel: GuardPanel, name: string): string {
    const value = this.host.localize(`{s:${name}}`, panel)
    return value.includes('{s:') ? '' : value
  }

  /** barSlots returns the bar's slots, kept while they are valid and newer than keep. */
  private barSlots(): BarSlot[] {
    // Keep the last look while it is new and whole; the game makes the pips
    // some time after the icons, so a slot without them looks again.
    const now = this.now()
    const last = this.bar
    if (
      last &&
      now - last.at < this.keep &&
      last.slots.every(
        (slot) =>
          slot.icon.IsValid() && slot.pips !== null && slot.pips.IsValid(),
      )
    )
      return last.slots
    const rows = barPipRows(this.hud)
    this.bar = {
      slots: barIcons(this.hud).map((icon) => {
        const pips = pipsOf(icon, rows)
        const unlocks = [1, 2, 3].map(
          (tier) => pips?.FindChildTraverse(`AbilityUnlock${tier}`) ?? null,
        )
        return {
          icon,
          readOnly: icon.FindChildTraverse('ReadOnlyPipsContainer'),
          pips,
          unlocks: unlocks.filter((unlock) => unlock !== null),
        }
      }),
      at: now,
    }
    return this.bar.slots
  }
}

/** undress gives a Tab icon back to the game. */
function undress(icon: TabIcon): void {
  // Undress only a dressed icon: delete the display's own panels, give the
  // icon its hover back, then clear the styles it marked.
  if (!unmark(icon.icon)) return
  for (const id of [DISPLAY_IMAGE, DISPLAY_COVER]) {
    const panel = icon.icon.FindChildTraverse(id)
    if (panel) panel.DeleteAsync(0)
  }
  icon.icon.ClearPanelEvent('onmouseover')
  if (icon.own) icon.own.style.opacity = null
  if (icon.ring && unmark(icon.ring)) icon.ring.style.visibility = null
  clearReadOnlyPips(icon.pips)
}

/**
 * drawBar draws upgrades on a bar slot: the frame, ring and cooldown shade
 * of the tier, the read-only pips, and the upgrade pips drawn flat, without
 * the game's cost, flash and train button, which follow its own idea of the
 * upgrades.
 */
function drawBar(
  { icon, readOnly, pips, unlocks }: BarSlot,
  upgrades: number,
): void {
  // Draw the frame of the tier, which an ability with a sub-ability makes
  // room in. Each panel's mark holds what it shows, so only a change, or a
  // panel the game rebuilt, is drawn.
  const tier = tierOf(upgrades)
  const withSub = icon.BHasClass('hasSubAbility')
  if (mark(icon, `${tier}${withSub ? ' sub' : ''}`)) {
    const frame =
      tier >= 2
        ? `ability_frame_passive_${tier}`
        : withSub
          ? 'ability_frame_passive_withsub_0'
          : 'ability_frame_passive_0'
    const background = firstWithClass(icon, 'ability_bg')
    if (background)
      background.style.backgroundImage = `url("${FRAMES}${frame}_psd.vtex")`
    const ring = firstWithClass(icon, 'ability_ring')
    if (ring) ring.style.visibility = tier > 0 ? 'visible' : 'collapse'
    const shade = icon.FindChildTraverse('cooldown_mask')
    if (shade)
      shade.style.opacityMask = `url("${FRAMES}ability_frame_${tier}.vsvg")`
  }
  drawReadOnlyPips(readOnly, upgrades)
  if (!pips) return

  // Show the pips in place of the train button, whatever the game takes.
  if (mark(pips, 'drawn')) {
    for (const id of ['TrainButton', 'LockedMsg']) {
      const part = pips.FindChildTraverse(id)
      if (part) part.style.visibility = 'collapse'
    }
    const row = pips.FindChildTraverse('AbilityUpgradePips')
    if (row) {
      row.style.visibility = 'visible'
      row.style.opacity = '1'
    }
  }

  // Draw each upgrade's pip flat, lit once bought.
  unlocks.forEach((unlock, index) => {
    if (mark(unlock, String(upgrades)))
      drawUnlock(unlock, bought(upgrades, index + 1))
  })
}

/**
 * drawUnlock draws one upgrade of the bar's pips: its pip flat in the
 * read-only pips' colors, without the game's wash, flash and cost.
 */
function drawUnlock(unlock: GuardPanel, on: boolean): void {
  // Clear the game's wash and flash.
  unlock.style.backgroundColor = NO_WASH
  unlock.style.opacity = '1'
  const flash = firstWithClass(unlock, 'upgradeReadyFlash')
  if (flash) flash.style.opacity = '0'

  // Draw the pip as a flat bar.
  const pip = firstWithClass(unlock, 'abilityLevelPip')
  if (pip) {
    pip.style.backgroundImage = 'none'
    pip.style.washColor = NO_WASH
    pip.style.opacity = '1'
    pip.style.width = '18px'
    pip.style.height = '6px'
    pip.style.borderRadius = '2px'
    pip.style.border = `1px solid ${PIP_BORDER}`
    pip.style.backgroundColor = on ? PIP_ON : PIP_OFF
  }

  // Hide the cost.
  const cost = firstWithClass(unlock, 'abilityUpgradeCostLabel')
  if (cost) cost.style.visibility = 'collapse'
}

/** PIP_STYLES are the styles drawBar sets on a bar pip. */
const PIP_STYLES = [
  'backgroundImage',
  'washColor',
  'opacity',
  'width',
  'height',
  'borderRadius',
  'border',
  'backgroundColor',
]

/** clearBar gives a bar slot back to the game: every style drawBar marked goes. */
function clearBar({ icon, readOnly, pips, unlocks }: BarSlot): void {
  // Clear the frame, ring and shade, and the read-only pips.
  if (unmark(icon)) {
    const background = firstWithClass(icon, 'ability_bg')
    if (background) background.style.backgroundImage = null
    const ring = firstWithClass(icon, 'ability_ring')
    if (ring) ring.style.visibility = null
    const shade = icon.FindChildTraverse('cooldown_mask')
    if (shade) shade.style.opacityMask = null
  }
  clearReadOnlyPips(readOnly)
  if (!pips) return

  // Clear the pips' row, then each upgrade's pip.
  if (unmark(pips)) {
    for (const id of ['TrainButton', 'LockedMsg']) {
      const part = pips.FindChildTraverse(id)
      if (part) part.style.visibility = null
    }
    const row = pips.FindChildTraverse('AbilityUpgradePips')
    if (row) {
      row.style.visibility = null
      row.style.opacity = null
    }
  }
  for (const unlock of unlocks) {
    if (!unmark(unlock)) continue
    unlock.style.backgroundColor = null
    unlock.style.opacity = null
    const flash = firstWithClass(unlock, 'upgradeReadyFlash')
    if (flash) flash.style.opacity = null
    const pip = firstWithClass(unlock, 'abilityLevelPip')
    if (pip) for (const name of PIP_STYLES) pip.style[name] = null
    const cost = firstWithClass(unlock, 'abilityUpgradeCostLabel')
    if (cost) cost.style.visibility = null
  }
}

/** drawReadOnlyPips colors an icon's read-only pips by upgrades; Pip1 is the first upgrade. */
function drawReadOnlyPips(box: GuardPanel | null, upgrades: number): void {
  if (!box || !mark(box, String(upgrades))) return
  for (let tier = 1; tier <= TIERS; tier++) {
    const pip = box.FindChildTraverse(`Pip${tier}`)
    if (pip)
      pip.style.backgroundColor = bought(upgrades, tier) ? PIP_ON : PIP_OFF
  }
}

/** clearReadOnlyPips gives an icon's read-only pips back to the game. */
function clearReadOnlyPips(box: GuardPanel | null): void {
  if (!box || !unmark(box)) return
  for (let tier = 1; tier <= TIERS; tier++) {
    const pip = box.FindChildTraverse(`Pip${tier}`)
    if (pip) pip.style.backgroundColor = null
  }
}

/** mark records value as drawn on panel, reporting whether it changed. */
function mark(panel: GuardPanel, value: string): boolean {
  if (panel.GetAttributeString(DRAWN_MARK, '') === value) return false
  panel.SetAttributeString(DRAWN_MARK, value)
  return true
}

/** unmark drops panel's drawn mark, reporting whether it had one. */
function unmark(panel: GuardPanel): boolean {
  if (panel.GetAttributeString(DRAWN_MARK, '') === '') return false
  panel.SetAttributeString(DRAWN_MARK, '')
  return true
}

/** bought reports whether upgrades holds tier. */
function bought(upgrades: number, tier: number): boolean {
  return (upgrades & (1 << tier)) !== 0
}

/** tierOf counts the upgrades bought after the unlock. */
function tierOf(upgrades: number): number {
  let tier = 0
  for (let n = 1; n <= TIERS; n++) if (bought(upgrades, n)) tier++
  return tier
}

/** firstWithClass returns the first panel under root with class name. */
function firstWithClass(root: GuardPanel, name: string): GuardPanel | null {
  return inOrder(root, BAR_DEPTH, (panel) => panel.BHasClass(name))[0] ?? null
}

/** barIcons returns the ability bar's icons under hud in slot order, by their ids. */
export function barIcons(hud: GuardPanel): GuardPanel[] {
  const signature = hud.FindChildTraverse('hud_signature')
  return signature
    ? inOrder(signature, BAR_DEPTH, (panel) =>
        /^slot_signature_[1-4]$/.test(panel.id),
      ).sort((a, b) => (a.id < b.id ? -1 : 1))
    : []
}

/** children returns panel's children. */
export function children(panel: GuardPanel): GuardPanel[] {
  const list: GuardPanel[] = []
  for (let index = 0; index < panel.GetChildCount(); index++) {
    const child = panel.GetChild(index)
    if (child) list.push(child)
  }
  return list
}

/**
 * inOrder returns the panels under root, at most depth levels down, that
 * wanted takes, in the order they sit, without looking inside them.
 */
export function inOrder(
  root: GuardPanel,
  depth: number,
  wanted: (panel: GuardPanel) => boolean,
): GuardPanel[] {
  const found: GuardPanel[] = []
  const walk = (panel: GuardPanel, levels: number) => {
    for (const child of children(panel)) {
      if (wanted(child)) found.push(child)
      else if (levels > 0) walk(child, levels - 1)
    }
  }
  walk(root, depth)
  return found
}

/** isPips reports whether panel is a row of upgrade pips. */
function isPips(panel: GuardPanel): boolean {
  return panel.paneltype === 'CitadelHudAbilityUpgradePips'
}

/** barPipRows returns every row of upgrade pips under the bar, in the order they sit. */
export function barPipRows(hud: GuardPanel): GuardPanel[] {
  const signature = hud.FindChildTraverse('hud_signature')
  return signature ? inOrder(signature, BAR_DEPTH, isPips) : []
}

/**
 * pipsOf returns the upgrade pips of a bar icon: the row inside it, the one
 * row beside it, or else its slot's row among rows, the bar's rows in the
 * order they sit. The game makes the rows apart from the icons and hides
 * them, so where they sit, not their place on screen, tells their slot.
 */
export function pipsOf(
  icon: GuardPanel,
  rows: readonly GuardPanel[],
): GuardPanel | null {
  // Look inside the icon first, then beside it, then by slot.
  const inside = inOrder(icon, BAR_DEPTH, isPips)
  if (inside.length !== 0) return inside[0]!
  const parent = icon.GetParent()
  const beside = parent ? inOrder(parent, BAR_DEPTH, isPips) : []
  if (beside.length === 1) return beside[0]!
  return rows[Number(icon.id.slice(-1)) - 1] ?? null
}
