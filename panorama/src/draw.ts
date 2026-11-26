// Drawing turns each mod's tree into stock panels. It runs no mod code: every
// property it writes comes from a typed field of the tree, and a node becomes
// one of the stock panel types. A node keeps its panel across style changes,
// so a transition animates them: a property the new style drops returns to
// its resting value, and only a property with none makes a new panel. Styles
// stack on a panel: the node's own, its hover's while the pointer is over it,
// and a phase over both: the enter style for one frame after the panel is
// made, a cycle's far end, or the exit style while a node that left the tree
// eases out before its panel goes.
//
// An input keeps the player's edit until the mod draws a new value for it,
// and sends each edit through Change, a text entry's and a slider's at most
// once each tenth of a second.
//
// A layout node is the one exception to stock panels: a host that has
// reviewed a mod's content addon passes Layouts, and the drawing loads the
// mod's layout into the node's panel; without Layouts the panel stays empty
// and the drawing logs it once.
// The same hosts load the mod's style sheets, which the root lists: the first
// into the mod's root panel, and each later one into a panel nested inside the
// one before, so the mod's classes match only its own panels and a later
// sheet wins. A node's classes change one by one, so a sheet's transitions
// run.
// A node that hides in chat hides while the game's chat box is open, which
// shows the chat history itself. The game marks the open box with a class
// and says nothing as it opens, so the drawing reads the class each tenth of
// a second while such a node shows.
// A route node draws nothing and hands its value to each of the mod's
// layouts. The drawing reaches the game only through the Panorama it is
// given, so a host's own interface script may draw the screens it receives
// another way.

import {
  Align,
  type Clip,
  type Clock,
  ClockFormat,
  type Cue,
  type Curve,
  Decoration,
  Easing,
  type Edges,
  Face,
  Flow,
  type Gradient,
  GradientShape,
  HeroLook,
  Kind,
  type Length,
  type Node,
  Overflow,
  type Region,
  Scaling,
  type Screen,
  type Slider,
  type Shadow,
  type Style,
  TextOverflow,
  type Transform,
  type Transition,
  type Tree,
  type UiInputEvent,
  Weight,
} from '../../src/wasm/ui/ui.pb.js'
import {
  base64Url,
  isRouteName,
  layoutDataAttribute,
  layoutDataChanged,
  layoutModAttribute,
  layoutNodeAttribute,
  layoutUnloadingAttribute,
  routeAttribute,
  utf8,
} from './data.js'
import { Voices } from './voice.js'
import { type MenuHost, Menus, type Surface } from './menu.js'
import { Documents, type Form } from './document.js'

export {
  actionCommand,
  isRouteName,
  layoutDataAttribute,
  layoutDataChanged,
  layoutModAttribute,
  layoutNodeAttribute,
  layoutUnloadingAttribute,
  maxActionPayload,
  routeAttribute,
} from './data.js'

/** Panel is the part of a game panel the drawing uses. */
export interface Panel {
  readonly id: string
  readonly paneltype: string
  hittest: boolean
  enabled: boolean
  text: string
  /** value is a native slider's position from 0 to 1. */
  value: number
  /** actuallayoutwidth is the panel's width in layout units. */
  readonly actuallayoutwidth?: number
  /** actualuiscale_x converts horizontal layout units to window pixels. */
  readonly actualuiscale_x?: number
  /** GetPositionWithinWindow returns the panel's origin in window pixels. */
  GetPositionWithinWindow?(): { x: number; y: number }
  /** checked reports whether a toggle button is checked. */
  checked: boolean
  visible: boolean
  readonly style: Record<string, string | null>
  IsValid(): boolean
  /** GetParent and BHasClass find and read the game's chat box. */
  GetParent?(): Panel | null
  BHasClass?(name: string): boolean
  /** FindChildrenWithClassTraverse finds the top bar's players by class. */
  FindChildrenWithClassTraverse?(name: string): Panel[]
  BHasKeyFocus?(): boolean
  BHasDescendantKeyFocus?(): boolean
  SetFocus?(): void
  GetChild(index: number): Panel | null
  FindChildTraverse(id: string): Panel | null
  MoveChildBefore(child: Panel, before: Panel): void
  MoveChildAfter(child: Panel, after: Panel): void
  SetImage(path: string): void
  SetPanelEvent(event: string, handler: () => void): void
  SetAttributeString(name: string, value: string): void
  AddClass(name: string): void
  RemoveClass(name: string): void
  BLoadLayout(
    path: string,
    overrideExisting: boolean,
    partial: boolean,
  ): boolean
  DeleteAsync(seconds: number): void
  SetDialogVariable(name: string, value: string): void
  /** AddOption, SetSelected and GetSelected hold a dropdown's options. */
  AddOption(option: Panel): void
  SetSelected(id: string): void
  GetSelected(): Panel | null
}

/**
 * Panorama is the part of the game's script API the drawing uses. CreatePanel
 * takes the properties a panel is made with only when there are some, since
 * the game refuses an undefined argument. PlaySound plays one of the game's
 * sound events and returns what StopSound stops, if anything; a host without
 * sound leaves them out.
 */
export interface Panorama {
  CreatePanel(
    type: string,
    parent: Panel,
    id: string,
    properties?: Record<string, string>,
  ): Panel
  DispatchEvent(event: string, ...values: unknown[]): void
  Msg(...values: unknown[]): void
  PlaySound?(sound: string): number | void
  StopSound?(playing: number): void
  /** MousePosition returns the cursor's position in window pixels. */
  MousePosition?(): { x: number; y: number }
  /** RegisterEventHandler binds a native event to this panel's lifetime. */
  RegisterEventHandler?(
    event: string,
    panel: Panel,
    handler: (...args: never[]) => boolean | void,
  ): void
  /**
   * Schedule runs callback after seconds. Without it, nothing eases in or
   * cycles and each edit sends at once.
   */
  Schedule?(seconds: number, callback: () => void): void
  /**
   * Localize returns token's text in the player's language, with the
   * dialog variables panel holds. Without it, a label shows its token.
   */
  Localize?(token: string, panel: Panel): string
}

/** Press reports a press on a mod's button. */
export type Press = (mod: string, node: string) => void

/** InputValue is the value a player gives one of a mod's inputs. */
export type InputValue = Exclude<
  UiInputEvent['value'],
  { case: undefined } | undefined
>

/** Change reports a change the player made to a mod's input. */
export type Change = (mod: string, node: string, value: InputValue) => void

/**
 * Layouts lets a host load the layouts of mods whose content it has reviewed.
 * A layout's script runs in the player's game, so only a host that checked
 * the mod's addon passes one.
 */
export interface Layouts {
  /**
   * url returns the URL of mod's layout at path, a path isLayoutPath accepts,
   * or null when the host does not load that mod's layouts.
   */
  url(mod: string, path: string): string | null
  /**
   * addon returns the name of the content addon that holds mod's files, which
   * an image names as {addon}, or null when the host does not know it.
   */
  addon(mod: string): string | null
  /**
   * unloaded runs after the drawing deletes one or more layouts' panels. A
   * layout's key binds replaced the host's for the whole game process, so the
   * host binds its keys again.
   */
  unloaded(): void
}

/**
 * AddonLayouts loads each mod's layouts from the content addon its tree names,
 * the one a Modlock session mounted: follow it to each screen before drawing
 * the screen. A mod whose tree names no addon has no layouts to load.
 */
export class AddonLayouts implements Layouts {
  /** addons holds the content addon each mod's tree last named, by mod. */
  private readonly addons = new Map<string, string>()

  /** follow records the addon each of screen's trees names. */
  follow(screen: Screen | null): void {
    for (const tree of screen?.trees ?? []) {
      this.addons.set(tree.mod ?? '', tree.addon ?? '')
    }
  }

  url(mod: string, path: string): string | null {
    const addon = this.addons.get(mod)
    return addon ? addonLayout(addon, path) : null
  }

  addon(mod: string): string | null {
    return this.addons.get(mod) || null
  }

  unloaded(): void {}
}

/**
 * addonLayout returns the URL of the layout at path, a path isLayoutPath
 * accepts, inside the content addon addon, where the build writes it.
 */
export function addonLayout(addon: string, path: string): string {
  return `file://{resources}/layout/${addon}/${path}`
}

/**
 * isLayoutPath reports whether path names a layout inside a mod's addon: one
 * or more segments of letters, digits, '_', '-' and '.', none of them "..",
 * ending in ".xml". The screen comes from the game server, so the drawing
 * checks the path before a host joins it to the addon's layout directory.
 */
export function isLayoutPath(path: string): boolean {
  return isAddonPath(path, '.xml')
}

/**
 * isSheetPath reports whether path names a style sheet inside a mod's addon,
 * as isLayoutPath does a layout, ending in ".css".
 */
export function isSheetPath(path: string): boolean {
  return isAddonPath(path, '.css')
}

/**
 * sheetLayout is the path of the layout the build writes beside the sheet at
 * path, which includes the sheet and nothing else.
 */
export function sheetLayout(path: string): string {
  return `${path}.xml`
}

/** isAddonPath reports whether path names a file ending in extension inside an addon. */
function isAddonPath(path: string, extension: string): boolean {
  return (
    path.endsWith(extension) &&
    path.split('/').every((part) => /^[\w.-]+$/.test(part) && part !== '..')
  )
}

/** addonPlace is the place in an image's path that names the mod's addon. */
const addonPlace = '{addon}'

/**
 * isAddonName reports whether name can name a content addon: one path
 * segment of letters, digits, '_', '-' and '.', other than "..".
 */
function isAddonName(name: string): boolean {
  return /^[\w.-]+$/.test(name) && name !== '..'
}

/** isClassName reports whether name can be a panel's class: letters, digits, '_' and '-'. */
function isClassName(name: string): boolean {
  return /^[\w-]+$/.test(name)
}

/** isSlug reports whether name can be a mod's slug: lowercase letters and digits joined by single hyphens. */
function isSlug(name: string): boolean {
  return /^[a-z0-9]+(-[a-z0-9]+)*$/.test(name)
}

/** isNodeId reports whether id can be a node's: up to 64 letters, digits, '_', '-', '.' and ':'. */
function isNodeId(id: string): boolean {
  return /^[\w.:-]{1,64}$/.test(id)
}

/**
 * pressCommand is the console command that presses node in mod's interface.
 * The game server hears it with the sending player, so any player's game can
 * press by running it.
 */
export function pressCommand(mod: string, node: string): string {
  if (isSlug(mod) && isNodeId(node)) {
    return `modlockpress ${mod} ${node}`
  }
  return ''
}

/**
 * inputCommand is the console command that sends value to the input node in
 * mod's interface, as pressCommand sends a press: text as unpadded base64url
 * of its UTF-8, cut to the 240 bytes the host takes, a number in decimal, a
 * toggle's state as 1 or 0, a choice as its index and a wheel step as
 * wheel:direction:fraction.
 */
export function inputCommand(
  mod: string,
  node: string,
  value: InputValue,
): string {
  if (isSlug(mod) && isNodeId(node)) {
    return `modlockinput ${mod} ${node} ${encodeInput(value)}`
  }
  return ''
}

/** encodeInput writes an input's value as inputCommand sends it. */
function encodeInput(value: InputValue): string {
  switch (value.case) {
    case 'text':
      return base64Url(utf8(value.value, maxInput))
    case 'number':
      return String(finite(value.value))
    case 'on':
      return value.value ? '1' : '0'
    case 'choice':
      return String(Math.trunc(finite(value.value)))
    case 'wheel':
      return `wheel:${finite(value.value.delta)}:${clamp(finite(value.value.fraction), 0, 1)}`
  }
}

/** maxInput is the most text the host takes from an entry, in bytes. */
const maxInput = 240

/**
 * maxUnanswered is how many sent values an input remembers until the mod
 * draws them back: a second of typing at the pace edits send.
 */
const maxUnanswered = 10

/** panelTypes maps each kind of node to the stock panel that draws it. */
const panelTypes: Record<Kind, string> = {
  [Kind.PANEL]: 'Panel',
  [Kind.LABEL]: 'Label',
  [Kind.IMAGE]: 'Image',
  [Kind.BUTTON]: 'Button',
  [Kind.LAYOUT]: 'Panel',
  [Kind.ROUTE]: 'Panel',
  [Kind.HERO_IMAGE]: 'CitadelHeroImage',
  [Kind.INPUT]: 'Panel',
  [Kind.AVATAR]: 'CitadelAvatarImage',
  [Kind.MENU]: 'Panel',
  [Kind.MENU_CLOSE]: 'Button',
  [Kind.DOCUMENT_INPUT]: 'Panel',
  [Kind.DOCUMENT_OUTPUT]: 'Label',
}

/** Control is the control an input draws, as its node names it. */
type Control = NonNullable<NonNullable<Node['input']>['case']>

/** controlTypes maps each input's control to the stock panel that draws it. */
const controlTypes: Record<Control, string> = {
  textEntry: 'TextEntry',
  slider: 'Slider',
  toggle: 'ToggleButton',
  dropdown: 'DropDown',
}

/**
 * entryLook is the game's own text box, which a text entry wears under its
 * mod's style. The stock styles that draw the box, and hide the placeholder
 * under typed text, reach no panel the drawing makes, so a bare entry would
 * show only small unboxed text.
 */
const entryLook: Record<string, string> = {
  width: '352px',
  height: '44px',
  padding: '8px 12px 8px 12px',
  backgroundColor: 'rgba(26, 26, 26, 0.900)',
  border: '2px solid rgba(198, 198, 198, 0.200)',
  borderRadius: '4px',
  color: 'rgba(255, 239, 215, 1.000)',
  fontSize: '18px',
  fontWeight: 'semi-bold',
  whiteSpace: 'nowrap',
  textOverflow: 'clip',
}

/** entryDisabled dims a text entry that takes no edits. */
const entryDisabled: Record<string, string> = {
  backgroundColor: 'rgba(26, 26, 26, 0.500)',
  color: 'rgba(255, 239, 215, 0.500)',
}

/** entryFocused brightens a text entry's border while it holds the keyboard. */
const entryFocused: Record<string, string> = {
  border: '2px solid rgba(255, 239, 215, 0.700)',
}

/**
 * Cycling is a cycle as the drawing runs it: key names the cycle the node
 * holds, style is its far end, seconds each way and count the swings there
 * and back, or zero to repeat.
 */
interface Cycling {
  key: string
  style: Record<string, string>
  seconds: number
  count: number
}

/**
 * Ticking is a label's clock as the drawing runs it: key names the clock the
 * mod sent, since is when it started counting from its seconds, in seconds
 * of wall time, and node is the label it fills.
 */
interface Ticking {
  key: string
  clock: Clock
  since: number
  node: Node
}

/** clockTick is how often a running clock writes its label, in seconds. */
const clockTick = 0.1

/**
 * followTick is how often the drawing reads the chat box and the voice marks
 * while a node that follows them is drawn, in seconds.
 */
const followTick = 0.1

/** nobody is the names heard where no node follows a voice. */
const nobody: ReadonlySet<string> = new Set()

/** CHAT_BOX is the id of the game's chat box, a CitadelChat, in its HUD. */
const CHAT_BOX = 'Chat'

/**
 * CHAT_OPEN is the class the game keeps on its chat box while the player
 * types in it and reads its history.
 */
export const CHAT_OPEN = 'ChatExpanded'

/**
 * findChatBox returns the game's chat box in the interface panel sits in, or
 * null where there is none.
 */
function findChatBox(panel: Panel): Panel | null {
  let top = panel
  for (let up = top.GetParent?.(); up; up = up.GetParent?.()) top = up
  const box = top.FindChildTraverse(CHAT_BOX)
  return box?.paneltype === 'CitadelChat' ? box : null
}

/** now returns the wall time in seconds. */
function now(): number {
  return Date.now() / 1000
}

/**
 * windClock returns the clock a label runs: the one it ran before while the
 * mod sends the same clock, or one started now.
 */
function windClock(node: Node, before: Ticking | null | undefined): Ticking {
  const clock = node.clock!
  const key = JSON.stringify(clock)
  return {
    key,
    clock,
    since: before?.key === key ? before.since : now(),
    node,
  }
}

/** clockTime returns the seconds a clock shows at time at. */
function clockTime(ticking: Ticking, at: number): number {
  const { seconds, up, paused } = ticking.clock
  const start = Math.max(0, finite(seconds))
  const passed = paused ? 0 : Math.max(0, at - ticking.since)
  return up ? start + passed : Math.max(0, start - passed)
}

/** clockRunning reports whether a clock's time still changes at time at. */
function clockRunning(ticking: Ticking, at: number): boolean {
  return (
    !ticking.clock.paused && (!!ticking.clock.up || clockTime(ticking, at) > 0)
  )
}

/**
 * clockText writes a clock's time in its format. A countdown rounds up and a
 * count up rounds down, so a countdown shows zero only once it ends.
 */
export function clockText(clock: Clock, time: number): string {
  const round = clock.up ? Math.floor : Math.ceil
  const pad = (value: number) => String(value).padStart(2, '0')
  switch (clock.format) {
    case ClockFormat.SECONDS:
      return String(round(time))
    case ClockFormat.TENTHS: {
      const tenths = round(Math.round(time * 1000) / 100)
      const whole = Math.floor(tenths / 10)
      const tail = `.${tenths % 10}`
      return whole < 60
        ? `${whole}${tail}`
        : `${Math.floor(whole / 60)}:${pad(whole % 60)}${tail}`
    }
    default: {
      const whole = round(time)
      return `${Math.floor(whole / 60)}:${pad(whole % 60)}`
    }
  }
}

/**
 * Tip is what a node's panel shows under the pointer: nothing, its text, its
 * item's shop card or its ability's window.
 */
type Tip = '' | 'text' | 'item' | 'ability'

/** Pending is a gathered edit waiting to be sent. */
interface Pending {
  readonly epoch: number | undefined
}

/**
 * Drawn is one node's panel, its parent, the properties it was made with as
 * JSON, and its style: the node's own, its hover's, the phase over both, its
 * exit style and how long it eases out, and what the panel holds now. It
 * keeps the node's pointer feedback for the panel's events, and which of its
 * tooltip and its item's shop card shows. An input records its control, the mod's value as last
 * drawn, whether an edit waits to send, and the values it sent, oldest
 * first, that the mod has not drawn back. A layout node's panel also
 * records the layout's URL, whether it loaded, whether it was told it is
 * leaving, and the data set on it. A root records its sheets' layout URLs
 * and the panels nested for the sheets after the first, and every panel its
 * classes.
 */
interface Drawn {
  menu?: Surface
  form?: Form
  panel: Panel
  parent: Panel
  made: string
  style: Record<string, string>
  hover: Record<string, string>
  phase: Record<string, string>
  exit: Record<string, string>
  exitSeconds: number
  cycle: Cycling | null
  written: Record<string, string>
  control: Control | null
  slider: Slider | undefined
  value: string
  /**
   * pending is a gathered edit not yet sent, with the epoch of the menu it was
   * made in, so an edit from a closed opening of the menu is never sent.
   */
  pending: Pending | null
  unanswered: string[]
  over: boolean
  focused: boolean
  tooltip: string
  item: number
  ability: number
  abilityPlayer: number
  tipped: Tip
  sound: string
  hoverSound: string
  disabled: boolean
  layout: string | null
  loaded: boolean
  unloading: boolean
  data: string
  sheets: string
  nested: Panel[]
  classes: string[]
  clock: Ticking | null
  hideInChat: boolean
  /** speaking is the player name whose voice shows the node, or ''. */
  speaking: string
}

/**
 * keptSounds is how many of a mod's sounds the drawing can stop: a sound
 * older than its last few cues has played out.
 */
const keptSounds = 8

/**
 * subtree returns every live loaded layout in previous whose panel is root's
 * or descends from it, including through nested sheet panels.
 */
function subtree(previous: ReadonlyMap<string, Drawn>, root: Drawn): Drawn[] {
  // Only live loaded layouts not already marked unloading need a notice.
  const live = [...previous.values()].filter(
    (each) => each.loaded && !each.unloading && each.panel.IsValid(),
  )
  if (live.length === 0) {
    return []
  }

  // Map each drawn panel and nested sheet panel to its parent panel.
  const parents = new Map<Panel, Panel>()
  for (const each of previous.values()) {
    parents.set(each.panel, each.parent)
    let host = each.panel
    for (const sheet of each.nested) {
      parents.set(sheet, host)
      host = sheet
    }
  }

  // Keep only the layouts hanging at or under root's panel.
  return live.filter((each) => {
    for (let at: Panel | undefined = each.panel; at; at = parents.get(at)) {
      if (at === root.panel) {
        return true
      }
    }
    return false
  })
}

/** Drawing keeps the panels of every mod's tree under one parent. */
export class Drawing {
  /** mods holds each mod's lifetime and drawn nodes; the root's id is empty. */
  private readonly mods = new Map<
    string,
    { generation: bigint; nodes: Map<string, Drawn> }
  >()

  /** routes holds each mod's routes, the value of each by name. */
  private readonly routes = new Map<string, Map<string, string>>()

  /**
   * cues holds, for each mod, the lifetime its cues belong to, the newest
   * cue played or passed over, and the sounds lately started, to stop.
   */
  private readonly cues = new Map<
    string,
    { generation: bigint; heard: number; playing: number[]; silenced: number }
  >()

  /** addons holds the content addon each mod's tree names, by mod. */
  private readonly addons = new Map<string, string>()

  /** ticking is true while a clock's next tick is scheduled. */
  private ticking = false

  /** chatBox is the game's chat box, once found. */
  private chatBox: Panel | null = null

  /** voices reads who the player hears, once a node follows a voice. */
  private voices: Voices | null = null

  /**
   * gated holds the drawn nodes the player's game shows or hides: those that
   * hide in chat and those that show while a player speaks.
   */
  private gated: Drawn[] = []

  /** following is true while the next read for gated is scheduled. */
  private following = false

  constructor(
    private readonly panorama: Panorama,
    private readonly parent: Panel,
    private readonly press: Press,
    private readonly layouts?: Layouts,
    private readonly change?: Change,
    menuHost?: MenuHost,
  ) {
    this.menus = new Menus(panorama, menuHost)
    this.documents = new Documents(panorama, menuHost)
  }

  private readonly menus: Menus
  private readonly documents: Documents

  /** scoreboard takes the game's scoreboard action, including key-up. */
  scoreboard(pressed: boolean): void {
    this.menus.scoreboard(pressed)
  }

  /** refused holds the files the drawing has logged as not loaded. */
  private readonly refused = new Set<string>()

  /** writing is true while the drawing writes an input's value. */
  private writing = false

  /** unloaded records that this draw deleted a layout's panel. */
  private unloaded = false

  /** changed records that this draw changed a loaded layout's data or routes. */
  private changed = false

  /**
   * release notifies every loaded layout that it is leaving, hides every
   * tooltip, shop card and ability window the mods' panels show, and stops
   * the sounds their cues started, as the player leaves: the game's windows,
   * sounds and layout scripts outlast the panels.
   */
  release(): void {
    // Drop locally owned input before notifying a departing layout.
    this.menus.close()
    this.documents.cancel()
    this.notifyUnloading(
      [...this.mods.values()].flatMap((drawn) => [...drawn.nodes.values()]),
    )
    for (const drawn of this.mods.values()) {
      for (const each of drawn.nodes.values()) {
        this.untip(each)
      }
    }
    for (const mod of this.cues.keys()) {
      this.silence(mod)
    }
  }

  /** draw makes the panels match screen, keeping each panel whose node stays. */
  draw(screen: Screen | null): void {
    // Delete the trees of mods the screen no longer holds.
    const trees = new Map(
      (screen?.trees ?? []).map((tree) => [tree.mod ?? '', tree]),
    )
    const dropped = [...this.mods].filter(([mod]) => !trees.has(mod))
    this.notifyUnloading(
      dropped.flatMap(([, drawn]) => [...drawn.nodes.values()]),
    )
    for (const [mod, drawn] of dropped) {
      for (const each of drawn.nodes.values()) {
        this.untip(each)
      }
      drawn.nodes.get('')?.panel.DeleteAsync(0)
      this.unloaded ||= [...drawn.nodes.values()].some(
        (each) => each.layout !== null,
      )
      this.mods.delete(mod)
      this.routes.delete(mod)
      this.addons.delete(mod)
    }

    // A mod gone from the screen stops its sounds and, back, plays only the
    // cues that come after.
    for (const mod of this.cues.keys()) {
      if (!trees.has(mod)) {
        this.silence(mod)
        this.cues.delete(mod)
      }
    }

    // Draw each mod's tree.
    for (const [mod, tree] of trees) {
      this.drawTree(mod, tree)
    }

    // Tell the layouts to read again, and let the host bind its keys again
    // once layouts are gone.
    if (this.changed) {
      this.changed = false
      this.panorama.DispatchEvent('PlaySoundEffect', layoutDataChanged)
    }
    if (this.unloaded) {
      this.unloaded = false
      this.layouts?.unloaded()
    }

    // Show the nodes that follow the chat box or a voice by what the game
    // shows now.
    this.gated = []
    for (const { nodes } of this.mods.values()) {
      for (const each of nodes.values()) {
        if (each.hideInChat || each.speaking) this.gated.push(each)
      }
    }
    this.follow()
    this.menus.finish()
    this.documents.check()
  }

  /**
   * follow shows or hides each gated node: one that hides in chat gives way
   * while the chat box is open, and one that follows a voice shows while its
   * player is heard. It reads again shortly while a node follows a voice, or
   * while one hides in chat and the chat box is found.
   */
  private follow(): void {
    // Nothing gated: nothing to read.
    const gated = this.gated.filter((each) => each.panel.IsValid())
    if (gated.length === 0) return

    // Read the chat box, finding it again once the game rebuilt it, and the
    // voice marks, each only where a node follows it.
    const chat = gated.some((each) => each.hideInChat)
    if (chat && !this.chatBox?.IsValid()) {
      this.chatBox = findChatBox(this.parent)
    }
    const open = chat && (this.chatBox?.BHasClass?.(CHAT_OPEN) ?? false)
    const voice = gated.some((each) => each.speaking !== '')
    const heard = voice
      ? (this.voices ??= new Voices(this.parent)).read(now())
      : nobody
    for (const each of gated) {
      const shown =
        !(each.hideInChat && open) &&
        (each.speaking === '' || heard.has(each.speaking))
      if (each.panel.visible !== shown) each.panel.visible = shown
    }

    // Read again shortly, while there is something to read.
    if (this.following || !this.panorama.Schedule) return
    if (!voice && !this.chatBox) return
    this.following = true
    this.panorama.Schedule(followTick, () => {
      this.following = false
      this.follow()
    })
  }

  /** drawTree draws one mod's nodes from its root down, in its region. */
  private drawTree(mod: string, tree: Tree): void {
    // Index the mod's nodes by id.
    const nodes = new Map(
      (tree.nodes ?? []).map((node) => [node.id ?? '', node]),
    )

    // Read the mod's routes; a layout that loads now starts with them all.
    const routes = new Map<string, string>()
    for (const node of nodes.values()) {
      if (node.kind === Kind.ROUTE && isRouteName(node.route ?? '')) {
        routes.set(node.route!, node.data ?? '')
      }
    }
    const sent = this.routes.get(mod) ?? new Map<string, string>()
    this.routes.set(mod, routes)
    this.addons.set(mod, tree.addon ?? '')

    // A clear followed by a redraw can share one sampled screen. Its new
    // generation still replaces the previous layout and its script state.
    const old = this.mods.get(mod)
    const generation = tree.generation ?? 0n
    if (old && old.generation !== generation) {
      this.notifyUnloading(old.nodes.values())
      for (const each of old.nodes.values()) {
        this.untip(each)
      }
      old.nodes.get('')?.panel.DeleteAsync(0)
      this.unloaded ||= [...old.nodes.values()].some(
        (each) => each.layout !== null,
      )
    }

    // Reuse panels only within the same UI lifetime.
    const previous =
      old?.generation === generation ? old.nodes : new Map<string, Drawn>()
    const drawn = new Map<string, Drawn>()
    const root = nodes.get('') ?? { id: '' }
    this.drawNode(mod, root, this.parent, previous, drawn, nodes, tree.region)

    // Hand each layout loaded before this draw the routes that changed, an
    // empty value for one the mod no longer sends.
    const changed = [...new Set([...sent.keys(), ...routes.keys()])].filter(
      (name) => sent.get(name) !== routes.get(name),
    )
    for (const [id, each] of drawn) {
      const before = previous.get(id)
      if (
        changed.length !== 0 &&
        before?.loaded &&
        before.panel === each.panel
      ) {
        for (const name of changed) {
          each.panel.SetAttributeString(
            routeAttribute(name),
            routes.get(name) ?? '',
          )
        }
        this.changed = true
      }
    }

    // A node no parent reaches is not drawn; its panel eases out to its exit
    // style, and a child of a panel that also leaves goes with it.
    const gone = [...previous]
      .filter(([id, old]) => !drawn.has(id) && old.panel.IsValid())
      .map(([, old]) => old)
    this.notifyUnloading(gone)
    const leaving = new Set(gone.map((old) => old.panel))
    for (const old of gone) {
      if (leaving.has(old.parent)) {
        this.untip(old)
        this.unloaded ||= old.layout !== null
      } else {
        this.leave(old)
      }
    }
    this.mods.set(mod, { generation, nodes: drawn })
    this.cue(mod, generation, root.cues ?? [])
  }

  /**
   * cue plays each of a mod's cues once: those numbered above the newest it
   * heard in the same lifetime, all of a new lifetime's, and none on the
   * first screen it reads, whose cues may be long over. A new lifetime stops
   * the old one's sounds.
   */
  private cue(mod: string, generation: bigint, cues: readonly Cue[]): void {
    // Note the first screen's cues; a new lifetime starts its count again.
    const newest = Math.max(0, ...cues.map((each) => each.number ?? 0))
    const memory = this.cues.get(mod)
    if (memory === undefined) {
      this.cues.set(mod, {
        generation,
        heard: newest,
        playing: [],
        silenced: 0,
      })
      return
    }
    if (memory.generation !== generation) {
      this.silence(mod)
      memory.generation = generation
      memory.heard = 0
    }

    // Play the cues not yet heard, oldest first, each after its delay. A
    // delayed cue plays only if its mod's sounds were not silenced meanwhile.
    const fresh = cues
      .filter((each) => (each.number ?? 0) > memory.heard)
      .sort((a, b) => (a.number ?? 0) - (b.number ?? 0))
    memory.heard = Math.max(memory.heard, newest)
    for (const each of fresh) {
      const delay = clamp(each.delay ?? 0, 0, 60)
      if (delay === 0 || !this.panorama.Schedule) {
        this.play(memory, each)
        continue
      }
      const silenced = memory.silenced
      this.panorama.Schedule(delay, () => {
        if (memory.silenced === silenced && this.cues.get(mod) === memory) {
          this.play(memory, each)
        }
      })
    }
  }

  /** play plays one cue, keeping its sound to stop and cutting a short one. */
  private play(memory: { playing: number[] }, each: Cue): void {
    // Play the sound, or nothing where the game cannot.
    const playing = this.panorama.PlaySound?.(each.sound ?? '')
    if (typeof playing !== 'number') {
      return
    }

    // Keep it to stop, and cut it after its seconds.
    memory.playing = [...memory.playing, playing].slice(-keptSounds)
    const seconds = each.seconds ?? 0
    if (seconds > 0) {
      this.panorama.Schedule?.(seconds, () => {
        if (memory.playing.includes(playing)) {
          memory.playing = memory.playing.filter((one) => one !== playing)
          this.panorama.StopSound?.(playing)
        }
      })
    }
  }

  /** silence stops the sounds a mod's cues lately started and those waiting. */
  private silence(mod: string): void {
    // Find the mod's cues; a mod that never played one has nothing to stop.
    const memory = this.cues.get(mod)
    if (memory === undefined) {
      return
    }

    // Stop what plays, and outdate the cues still waiting to play.
    for (const playing of memory.playing) {
      this.panorama.StopSound?.(playing)
    }
    memory.playing = []
    memory.silenced++
  }

  /**
   * notifyUnloading marks each live loaded layout in nodes as unloading and
   * wakes the layout library at once, before DeleteAsync invalidates the
   * panel or its ancestor. ../LayoutUnload.tla checks this lifecycle and
   * names the client behavior it assumes.
   */
  private notifyUnloading(nodes: Iterable<Drawn>): void {
    let marked = false
    for (const each of nodes) {
      if (each.loaded && !each.unloading && each.panel.IsValid()) {
        each.unloading = true
        each.panel.SetAttributeString(layoutUnloadingAttribute, '1')
        marked = true
      }
    }
    if (marked) {
      this.panorama.DispatchEvent('PlaySoundEffect', layoutDataChanged)
    }
  }

  /**
   * delete deletes a drawn node's panel after seconds, and its tooltip, and
   * notes a layout's.
   */
  private delete(drawn: Drawn, seconds = 0): void {
    this.untip(drawn)
    drawn.panel.DeleteAsync(seconds)
    this.unloaded ||= drawn.layout !== null
  }

  /**
   * leave deletes the panel of a node that left the tree, once it has eased
   * to its exit style; it takes the pointer no more meanwhile.
   */
  private leave(drawn: Drawn): void {
    // A node without an exit style goes at once.
    if (Object.keys(drawn.exit).length === 0 || !drawn.exitSeconds) {
      this.delete(drawn)
      return
    }

    // Ease out, then go.
    drawn.over = false
    drawn.focused = false
    drawn.phase = drawn.exit
    this.paint(drawn)
    drawn.panel.hittest = false
    this.delete(drawn, drawn.exitSeconds)
  }

  /**
   * current returns the node as last drawn while it still holds panel, which
   * the game still holds.
   */
  private current(mod: string, id: string, panel: Panel): Drawn | undefined {
    const drawn = this.mods.get(mod)?.nodes.get(id)
    return drawn?.panel === panel && panel.IsValid() ? drawn : undefined
  }

  /**
   * paint writes the style the node's panel should hold now, the hover's over
   * the node's while the pointer is over it and an entry's focus ring while it
   * holds the keyboard, returning each property it no longer sets to its
   * resting value.
   */
  private paint(drawn: Drawn): void {
    const target = {
      ...drawn.style,
      ...(drawn.over ? drawn.hover : {}),
      ...(drawn.focused ? entryFocused : {}),
      ...drawn.phase,
    }
    for (const name of Object.keys(drawn.written)) {
      if (!(name in target) && name in rest) {
        this.write(drawn.panel, name, rest[name]!)
      }
    }
    for (const [name, value] of Object.entries(target)) {
      if (drawn.written[name] !== value) {
        this.write(drawn.panel, name, value)
      }
    }
    drawn.written = target
  }

  /**
   * point answers the pointer entering or leaving a node's panel: it draws or
   * lifts the hover style, plays the hover sound and shows or hides the
   * tooltip. The node is looked up as last drawn, so the panel's events follow
   * its later changes.
   */
  private point(mod: string, id: string, over: boolean): void {
    // Skip a repeated event and a panel the game has dropped.
    const drawn = this.mods.get(mod)?.nodes.get(id)
    if (!drawn || drawn.over === over || !drawn.panel.IsValid()) {
      return
    }

    // Tick, show the ability's window, the item's shop card or the tooltip,
    // or hide them, then repaint in the new state. The card's second value is
    // the game's default, as its own shop hover sends.
    drawn.over = over
    if (over && drawn.hoverSound) {
      this.panorama.PlaySound?.(drawn.hoverSound)
    }
    if (over) {
      this.tip(drawn)
    } else {
      this.untip(drawn)
    }
    this.paint(drawn)
  }

  /** focus draws or lifts an entry's focus ring as it takes or loses the keyboard. */
  private focus(mod: string, id: string, panel: Panel, focused: boolean): void {
    const drawn = this.current(mod, id, panel)
    if (!drawn) return
    // An entry that lets the keyboard go commits what it shows.
    if (!focused) this.commit(mod, id, drawn)
    if (drawn.focused !== focused) {
      drawn.focused = focused
      this.paint(drawn)
    }
  }

  /**
   * tip shows what a node's panel shows under the pointer: its ability's
   * window, else its item's shop card, else its tooltip.
   */
  private tip(drawn: Drawn): void {
    if (drawn.ability) {
      this.panorama.DispatchEvent(
        'CitadelShowAbilityDetailsTooltip',
        drawn.panel,
        drawn.ability,
        drawn.abilityPlayer,
      )
      drawn.tipped = 'ability'
    } else if (drawn.item) {
      this.panorama.DispatchEvent(
        'CitadelShowModHoverTooltip',
        drawn.panel,
        drawn.item,
        0,
      )
      drawn.tipped = 'item'
    } else if (drawn.tooltip) {
      this.panorama.DispatchEvent(
        'UIShowTextTooltip',
        drawn.panel,
        drawn.tooltip,
      )
      drawn.tipped = 'text'
    }
  }

  /** untip hides the tooltip, shop card or ability window a node's panel shows. */
  private untip(drawn: Drawn): void {
    const tip = drawn.tipped
    drawn.tipped = ''
    if (tip === 'ability') {
      this.panorama.DispatchEvent(
        'CitadelHideAbilityDetailsTooltip',
        drawn.panel,
      )
    } else if (tip === 'item') {
      this.panorama.DispatchEvent('CitadelHideModHoverTooltip', drawn.panel)
    } else if (tip === 'text') {
      this.panorama.DispatchEvent('UIHideTextTooltip', drawn.panel)
    }
  }

  /** settle lifts a new panel's enter style, so it eases to the node's. */
  private settle(mod: string, id: string, panel: Panel): void {
    const drawn = this.current(mod, id, panel)
    if (drawn && !drawn.cycle) {
      drawn.phase = {}
      this.paint(drawn)
    }
  }

  /**
   * swing moves a cycling node to the far end of its cycle on even steps and
   * back on odd ones, until it has swung count times or its panel or cycle
   * changes.
   */
  private swing(
    mod: string,
    id: string,
    panel: Panel,
    key: string,
    step: number,
  ): void {
    // Stop once the node holds another panel or cycle.
    const drawn = this.current(mod, id, panel)
    const cycle = drawn?.cycle
    if (!drawn || cycle?.key !== key) {
      return
    }

    // Swing, and come back for the next step.
    const done = cycle.count !== 0 && step >= cycle.count * 2
    drawn.phase = done || step % 2 === 1 ? {} : cycle.style
    this.paint(drawn)
    if (!done) {
      this.panorama.Schedule?.(cycle.seconds, () =>
        this.swing(mod, id, panel, key, step + 1),
      )
    }
  }

  /**
   * edit answers the player editing an input: it sends the value at once, or
   * a tenth of a second after the first of a run of edits to a text entry or
   * slider. The drawing's own writes and a disabled input send nothing.
   */
  private edit(mod: string, id: string, panel: Panel): void {
    // Skip the drawing's own writes and an input that takes no changes.
    const drawn = this.current(mod, id, panel)
    if (
      this.writing ||
      !drawn ||
      drawn.disabled ||
      !this.change ||
      (drawn.menu && !this.menus.accepts(drawn.menu))
    ) {
      return
    }

    // Send a toggle or a choice at once, and gather a run of other edits.
    const gathers = drawn.control === 'textEntry' || drawn.control === 'slider'
    if (!gathers || !this.panorama.Schedule) {
      this.send(mod, id, drawn)
    } else if (!drawn.pending) {
      drawn.pending = { epoch: drawn.menu?.epoch }
      this.panorama.Schedule(0.1, () => {
        const later = this.current(mod, id, panel)
        if (later) this.commit(mod, id, later)
      })
    }
  }

  /**
   * commit sends drawn's gathered edit now, unless the menu it was made in
   * has closed since.
   */
  private commit(mod: string, id: string, drawn: Drawn): void {
    const pending = drawn.pending
    if (!pending) return
    drawn.pending = null
    if (pending.epoch === drawn.menu?.epoch) this.send(mod, id, drawn)
  }

  /**
   * flush commits every gathered edit of mod's interface, so a press that
   * follows reaches the mod after the values the player sees.
   */
  private flush(mod: string): void {
    for (const [id, drawn] of this.mods.get(mod)?.nodes ?? []) {
      this.commit(mod, id, drawn)
    }
  }

  /** send sends an input's value as its panel holds it, playing its sound. */
  private send(mod: string, id: string, drawn: Drawn): void {
    if (drawn.menu && !this.menus.accepts(drawn.menu)) return
    const value = this.attempt('read an input', () =>
      readInput(drawn.control, drawn.panel, drawn.slider),
    )
    if (value) {
      drawn.unanswered.push(encodeInput(value))
      drawn.unanswered.splice(0, drawn.unanswered.length - maxUnanswered)
      if (drawn.sound) {
        this.panorama.PlaySound?.(drawn.sound)
      }
      this.change?.(mod, id, value)
    }
  }

  /**
   * answer shows the mod's new value of an input unless it only draws back a
   * value the panel sent: the player has typed on since, and the round trip
   * would erase what they typed while it ran.
   */
  private answer(drawn: Drawn, node: Node, previous?: Drawn): void {
    // Drop an echo and the older sends it answers; anything else is the
    // mod's own value. A new range maps that value to a new thumb position,
    // so it must be written even when the value itself was sent earlier.
    const value = nodeInput(node)
    const rangeChanged =
      drawn.slider &&
      (finite(drawn.slider.min) !== finite(previous?.slider?.min) ||
        finite(drawn.slider.max) !== finite(previous?.slider?.max) ||
        finite(drawn.slider.step) !== finite(previous?.slider?.step))
    const sent =
      value && !rangeChanged ? drawn.unanswered.indexOf(encodeInput(value)) : -1
    if (sent >= 0) {
      drawn.unanswered.splice(0, sent + 1)
      return
    }
    drawn.unanswered.length = 0
    this.writeInput(drawn.panel, node)
  }

  /** writeInput shows the mod's value of an input in its panel. */
  private writeInput(panel: Panel, node: Node): void {
    this.writing = true
    this.attempt('write an input', () => {
      switch (node.input?.case) {
        case 'textEntry':
          panel.text = node.input.value.text ?? ''
          showPlaceholder(panel)
          break
        case 'slider':
          panel.value = sliderFraction(node.input.value)
          break
        case 'toggle':
          panel.checked = !!node.input.value.on
          break
        case 'dropdown':
          panel.SetSelected(`option${node.input.value.selected ?? 0}`)
      }
    })
    this.writing = false
  }

  /** listen sends each edit to an input's new panel, with its options. */
  private listen(mod: string, id: string, panel: Panel, node: Node): void {
    const edited = () => this.edit(mod, id, panel)
    switch (node.input?.case) {
      case 'textEntry':
        panel.SetPanelEvent('ontextentrychange', () => {
          showPlaceholder(panel)
          edited()
        })
        panel.SetPanelEvent('onfocus', () => this.focus(mod, id, panel, true))
        panel.SetPanelEvent('onblur', () => this.focus(mod, id, panel, false))
        break
      case 'slider':
        panel.SetPanelEvent('onvaluechanged', edited)
        this.panorama.RegisterEventHandler?.('ScrollUp', panel, () =>
          this.wheel(mod, id, panel, 1),
        )
        this.panorama.RegisterEventHandler?.('ScrollDown', panel, () =>
          this.wheel(mod, id, panel, -1),
        )
        break
      case 'toggle':
        panel.SetPanelEvent('onactivate', edited)
        break
      case 'dropdown':
        // A dropdown holds a label for each option, named by its index.
        for (const [index, option] of (
          node.input.value.options ?? []
        ).entries()) {
          const label = this.panorama.CreatePanel(
            'Label',
            panel,
            `option${index}`,
            {
              text: option,
            },
          )
          this.attempt('add an option', () => panel.AddOption(label))
        }
        panel.SetPanelEvent('oninputsubmit', edited)
    }
  }

  /** wheel sends a scoped step with the pointer anchor, without seeking. */
  private wheel(mod: string, id: string, panel: Panel, delta: number): boolean {
    // A removed, disabled or opted-out slider leaves the event unhandled.
    const drawn = this.current(mod, id, panel)
    if (
      !drawn?.slider?.wheel ||
      drawn.disabled ||
      !this.change ||
      (drawn.menu && !this.menus.accepts(drawn.menu))
    ) {
      return false
    }

    // Both positions are window pixels; the width includes the UI scale.
    const mouse = this.panorama.MousePosition?.()
    const origin = panel.GetPositionWithinWindow?.()
    const width = (panel.actuallayoutwidth ?? 0) * (panel.actualuiscale_x ?? 1)
    if (!mouse || !origin || !(width > 0) || !Number.isFinite(mouse.x)) {
      return false
    }

    // Wheel input has its own typed payload and never joins value echoes.
    this.change(mod, id, {
      case: 'wheel',
      value: { delta, fraction: clamp((mouse.x - origin.x) / width, 0, 1) },
    })
    return true
  }

  /**
   * labelText returns a label's text: its token's in the player's language,
   * or its own. A clock's time stands in for the label's own text, or fills
   * its token's {s:clock} place.
   */
  private labelText(panel: Panel, node: Node, clock: Ticking | null): string {
    // A label without a token, or a host without the game's text, shows the
    // label's own text.
    const time = clock ? clockText(clock.clock, clockTime(clock, now())) : null
    const token = node.token ?? ''
    if (!/^#[\w.-]{1,127}$/.test(token) || !this.panorama.Localize) {
      return time ?? (node.text || token)
    }

    // Fill the token's places, then look it up.
    const variables = Object.entries(node.variables ?? {})
    if (time !== null) {
      variables.push(['clock', time])
    }
    for (const [name, value] of variables) {
      this.attempt('fill a token', () => panel.SetDialogVariable(name, value))
    }
    return this.panorama.Localize(token, panel)
  }

  /**
   * tick schedules the next write of every running clock's label, unless one
   * is scheduled, and goes on while any clock runs.
   */
  private tick(): void {
    if (this.ticking || !this.panorama.Schedule) {
      return
    }
    this.ticking = true
    this.panorama.Schedule(clockTick, () => {
      // Rewrite each running clock's label, noting whether any still runs.
      this.ticking = false
      const at = now()
      let running = false
      for (const { nodes } of this.mods.values()) {
        for (const each of nodes.values()) {
          if (!each.clock || !each.panel.IsValid()) {
            continue
          }
          const text = this.labelText(each.panel, each.clock.node, each.clock)
          if (each.panel.text !== text) {
            each.panel.text = text
          }
          running ||= clockRunning(each.clock, at)
        }
      }
      if (running) {
        this.tick()
      }
    })
  }

  /**
   * imagePath returns an image's path with mod's content addon in place of
   * {addon}, or no image when the path names the addon and none is known.
   */
  private imagePath(mod: string, path: string): string {
    if (!path.includes(addonPlace)) {
      return path
    }
    const addon = this.layouts
      ? this.layouts.addon(mod)
      : this.addons.get(mod) || null
    return addon && isAddonName(addon) ? path.split(addonPlace).join(addon) : ''
  }

  /** activate presses a button that is not disabled, playing its sound. */
  private activate(mod: string, id: string, panel: Panel): void {
    // A press counts only on a live, enabled button in an open menu.
    const drawn = this.current(mod, id, panel)
    if (
      !drawn ||
      drawn.disabled ||
      (drawn.menu && !this.menus.accepts(drawn.menu))
    ) {
      return
    }
    if (drawn?.sound) {
      this.panorama.PlaySound?.(drawn.sound)
    }

    // Edits the player typed reach the mod before the press that follows them.
    this.flush(mod)
    this.press(mod, id)
  }

  /**
   * drawNode draws node under parent, then its children in order. A root
   * drawn in a region fills that box instead of the screen.
   */
  private drawNode(
    mod: string,
    node: Node,
    parent: Panel,
    previous: Map<string, Drawn>,
    drawn: Map<string, Drawn>,
    nodes: Map<string, Node>,
    region?: Region,
    parentFlow?: Flow,
    surface?: Surface,
  ): Panel {
    // Resolve the node's id, kind, and the URL and data of its layout.
    const id = node.id ?? ''
    const kind = id === '' ? Kind.PANEL : (node.kind ?? Kind.PANEL)
    const path = node.layout ?? ''
    const layout =
      kind === Kind.LAYOUT && isLayoutPath(path)
        ? (this.layouts?.url(mod, path) ?? null)
        : null
    if (kind === Kind.LAYOUT && !layout) {
      this.refuse(mod, 'layout', path)
    }
    const data = node.data ?? ''
    const sheets =
      id === '' && this.layouts
        ? (node.sheets ?? [])
            .filter(isSheetPath)
            .flatMap((sheet): [string, string][] => {
              const url = this.layouts!.url(mod, sheetLayout(sheet))
              if (url === null) this.refuse(mod, 'style sheet', sheet)
              return url === null ? [] : [[sheet, url]]
            })
        : []
    const sheeted = JSON.stringify(sheets)

    // Resolve its panel type, the properties it is made with, and its
    // options, which a dropdown also makes its panel with.
    const control = kind === Kind.INPUT ? (node.input?.case ?? null) : null
    const type = control ? controlTypes[control] : (panelTypes[kind] ?? 'Panel')
    const properties = madeWith(kind, node)
    const made = JSON.stringify([
      kind,
      properties,
      node.documentInput?.placeholder,
      node.input?.case === 'dropdown' ? node.input.value.options : null,
    ])

    // Resolve its style and the styles laid over it, with the transition
    // that eases between them.
    const style: Record<string, string> = {
      ...(control === 'textEntry' ? entryLook : {}),
      ...(kind === Kind.DOCUMENT_INPUT
        ? { width: '360px', height: 'fit-children', flowChildren: 'down' }
        : {}),
      ...(kind === Kind.DOCUMENT_OUTPUT
        ? {
            ...entryLook,
            height: '130px',
            whiteSpace: 'normal',
            overflow: 'squish scroll',
          }
        : {}),
      ...(control === 'textEntry' && node.disabled ? entryDisabled : {}),
      ...styleOf(node.style, id === '', parentFlow),
      ...(region ? placed(region) : {}),
    }
    const hover = layerOf(node.hover, style, parentFlow)
    const enter = layerOf(node.enter, style, parentFlow)
    const exit = layerOf(node.exit, style, parentFlow)
    const cycle = cyclingOf(node, style, parentFlow)
    const transition = node.style?.transition ?? cycleTransition(cycle)
    Object.assign(
      style,
      transitionOf(transition, style, {
        ...hover,
        ...enter,
        ...exit,
        ...cycle?.style,
      }),
    )

    // Find the node's last panel and the style it now wants, with the hover
    // style over it while the pointer stays on it.
    const old = previous.get(id)
    const over = old?.over ?? false
    const target = { ...style, ...(over ? hover : {}), ...old?.phase }

    // Reuse the panel when its type, parent, properties and layout stay and
    // each property it holds that the new style drops has a resting value to
    // return to; or make one.
    let panel = old?.panel
    let written = old?.written ?? {}
    let loaded = old?.loaded ?? false
    let given = old?.data ?? ''
    const kept =
      old &&
      !old.unloading &&
      old.parent === parent &&
      old.made === made &&
      old.layout === layout &&
      old.sheets === sheeted &&
      Object.keys(written).every((name) => name in target || name in rest)
    if (!panel || !kept || !panel.IsValid() || panel.paneltype !== type) {
      if (old?.panel.IsValid()) {
        this.notifyUnloading(subtree(previous, old))
        this.delete(old)
      }
      const name =
        id === '' ? `ModlockUi_${mod.replace(/\W/g, '_')}` : 'ModlockUiNode'
      panel = properties
        ? this.panorama.CreatePanel(type, parent, name, properties)
        : this.panorama.CreatePanel(type, parent, name)
      written = {}
      loaded = false
      given = ''
      if (kind === Kind.BUTTON) {
        const button = panel
        panel.SetPanelEvent('onactivate', () => this.activate(mod, id, button))
      } else if (kind === Kind.MENU_CLOSE) {
        const button = panel
        panel.SetPanelEvent('onactivate', () => {
          const menu = this.current(mod, id, button)?.menu
          if (menu) this.menus.cancel(menu)
        })
      }
      if (control) {
        this.listen(mod, id, panel, node)
      }
      if (!layout) {
        panel.SetPanelEvent('onmouseover', () => this.point(mod, id, true))
        panel.SetPanelEvent('onmouseout', () => this.point(mod, id, false))
      }
    }

    // Load the node's layout with its data, or hand a loaded layout its new
    // data. A layout whose addon has not mounted yet loads on a later draw.
    if (layout && !loaded) {
      panel.SetAttributeString(layoutModAttribute, mod)
      panel.SetAttributeString(layoutNodeAttribute, id)
      panel.SetAttributeString(layoutDataAttribute, data)
      for (const [name, value] of this.routes.get(mod) ?? []) {
        panel.SetAttributeString(routeAttribute(name), value)
      }
      loaded = panel.BLoadLayout(layout, false, false)
      if (!loaded) {
        this.panorama.Msg(`[Modlock] ${mod} layout ${path} is unavailable`)
      }
    } else if (layout && data !== given) {
      panel.SetAttributeString(layoutDataAttribute, data)
      this.changed = true
    }

    // A new root loads its sheets; the panels nested for later sheets pass
    // the pointer through and lay the root's children out as the root does.
    const fresh = old?.panel !== panel
    const nested = fresh ? this.loadSheets(mod, panel, sheets) : old.nested
    if (fresh || old.style.flowChildren !== style.flowChildren) {
      for (const each of nested) {
        this.write(each, 'flowChildren', style.flowChildren ?? 'none')
      }
    }

    // Add the node's new classes and remove the ones it dropped.
    const classes = [...new Set(node.classes ?? [])].filter(isClassName)
    const had = fresh ? [] : old.classes
    for (const name of had) {
      if (!classes.includes(name)) {
        panel.RemoveClass(name)
      }
    }
    for (const name of classes) {
      if (!had.includes(name)) {
        panel.AddClass(name)
      }
    }

    // A kept panel keeps its phase while its cycle stays, and its pending
    // edit; a new one starts from its enter style.
    const phase =
      !fresh && old.cycle?.key === cycle?.key
        ? old.phase
        : fresh && this.panorama.Schedule
          ? enter
          : {}
    const value = control ? JSON.stringify(node.input?.value) : ''
    const each: Drawn = {
      menu: surface,
      panel,
      parent,
      made,
      style,
      hover,
      phase,
      exit,
      exitSeconds: transition?.seconds
        ? clamp(transition.seconds + (transition.delay ?? 0), 0, 120)
        : 0,
      cycle,
      written,
      control,
      slider:
        node.input?.case === 'slider' ? { ...node.input.value } : undefined,
      value,
      pending: fresh ? null : old.pending,
      unanswered: fresh ? [] : old.unanswered,
      over,
      focused: !fresh && old.focused,
      tooltip: node.tooltip ?? '',
      item: node.item ?? 0,
      ability: node.ability ?? 0,
      abilityPlayer: node.abilityPlayer ?? 0,
      tipped: old?.panel === panel ? old.tipped : '',
      sound: soundOf(node.sound),
      hoverSound: soundOf(node.hoverSound),
      disabled:
        (kind === Kind.BUTTON || kind === Kind.INPUT) && !!node.disabled,
      layout,
      loaded,
      unloading: false,
      data,
      sheets: sheeted,
      nested,
      classes,
      clock:
        kind === Kind.LABEL && node.clock
          ? windClock(node, fresh ? null : old.clock)
          : null,
      hideInChat: !!node.hideInChat,
      speaking: node.speaking ?? '',
    }
    drawn.set(id, each)

    // A kept panel that no longer follows the chat box or a voice shows again.
    const followed = (node: Drawn) => node.hideInChat || node.speaking !== ''
    if (!fresh && followed(old) && !followed(each)) panel.visible = true

    // Write the node's content and the style properties that changed. A
    // layout's own root decides whether it takes the pointer; a node takes it
    // to be pressed, to answer hover, or to scroll.
    if (!layout) {
      panel.hittest =
        kind === Kind.BUTTON ||
        kind === Kind.MENU_CLOSE ||
        kind === Kind.DOCUMENT_OUTPUT ||
        kind === Kind.INPUT ||
        Object.keys(hover).length !== 0 ||
        !!each.tooltip ||
        !!each.item ||
        !!each.ability ||
        !!each.hoverSound ||
        /scroll/.test(style.overflow ?? '')
    }
    if (kind === Kind.BUTTON || kind === Kind.INPUT) {
      panel.enabled = !each.disabled
    }
    if (kind === Kind.LABEL) {
      panel.text = this.labelText(panel, node, each.clock)
      if (each.clock && clockRunning(each.clock, now())) {
        this.tick()
      }
    } else if (kind === Kind.DOCUMENT_INPUT && node.documentInput) {
      each.form = this.documents.form(
        panel,
        mod,
        id,
        node.documentInput,
        !!node.disabled,
        fresh ? undefined : old.form,
        (form) =>
          this.current(mod, id, panel)?.form === form &&
          (!surface || this.menus.accepts(surface)),
      )
    } else if (kind === Kind.DOCUMENT_OUTPUT) {
      if (panel.text !== (node.documentText ?? ''))
        panel.text = node.documentText ?? ''
    } else if (kind === Kind.IMAGE) {
      panel.SetImage(this.imagePath(mod, node.image ?? ''))
    } else if (control && (fresh || old.value !== value)) {
      this.answer(each, node, old)
    }
    if (
      control === 'textEntry' &&
      (fresh ||
        old.style.color !== style.color ||
        old.style.fontSize !== style.fontSize)
    ) {
      this.dressPlaceholder(panel, style)
    }
    this.paint(each)
    if (
      (each.tipped === 'text' && !each.tooltip) ||
      (each.tipped === 'item' && each.item !== old?.item) ||
      (each.tipped === 'ability' &&
        (each.ability !== old?.ability ||
          each.abilityPlayer !== old.abilityPlayer))
    ) {
      // What changed under the pointer shows at once.
      this.untip(each)
      if (each.over) {
        this.tip(each)
      }
    }

    // A new panel eases from its enter style on the next frame, and a new
    // cycle starts swinging.
    const shown = panel
    if (fresh && Object.keys(enter).length !== 0) {
      this.panorama.Schedule?.(0, () => this.settle(mod, id, shown))
    }
    if (cycle && (fresh || old.cycle?.key !== cycle.key)) {
      this.panorama.Schedule?.(0, () =>
        this.swing(mod, id, shown, cycle.key, 0),
      )
    }

    // Draw each child once, in order; a child seen before would make a cycle.
    // A layout's panels come from its file, not from the tree, and a root's
    // children hang from the panel of its last sheet.
    const menu =
      kind === Kind.MENU && node.menu && !surface
        ? this.menus.mount(
            mod,
            id,
            panel,
            node.menu,
            style.flowChildren ?? 'none',
          )
        : undefined
    if (menu) menu.dismiss = () => this.documents.dismiss(menu.content)
    const holder = menu?.content ?? nested[nested.length - 1] ?? panel
    let last: Panel | undefined
    for (const child of kind === Kind.LAYOUT ||
    kind === Kind.DOCUMENT_INPUT ||
    kind === Kind.DOCUMENT_OUTPUT
      ? []
      : (node.children ?? [])) {
      const childNode = nodes.get(child)
      if (!childNode || drawn.has(child) || child === '') {
        continue
      }
      const childPanel = this.drawNode(
        mod,
        childNode,
        holder,
        previous,
        drawn,
        nodes,
        undefined,
        node.style?.flow,
        menu ?? surface,
      )
      if (last) {
        holder.MoveChildAfter(childPanel, last)
      } else if (holder.GetChild(0) && holder.GetChild(0) !== childPanel) {
        holder.MoveChildBefore(childPanel, holder.GetChild(0)!)
      }
      last = childPanel
    }
    return panel
  }

  /**
   * refuse logs once that the host gave mod's file at path, of the given kind,
   * no URL, so the panel draws empty.
   */
  private refuse(mod: string, kind: string, path: string): void {
    const file = `${mod}/${path}`
    if (this.refused.has(file)) return
    this.refused.add(file)
    this.panorama.Msg(`[Modlock] ${mod} ${kind} ${path} has no content to load`)
  }

  /**
   * loadSheets loads each of a new root's sheets, given as its path and its
   * layout's URL: the first into the root's panel, each later one into a
   * panel nested in the one before. It returns the nested panels. A sheet the
   * game cannot load leaves the panels drawn with their own styles.
   */
  private loadSheets(
    mod: string,
    root: Panel,
    sheets: readonly (readonly [string, string])[],
  ): Panel[] {
    const nested: Panel[] = []
    let into = root
    for (const [index, [path, url]] of sheets.entries()) {
      if (index !== 0) {
        into = this.panorama.CreatePanel('Panel', into, 'ModlockUiSheet')
        nested.push(into)
      }
      if (!into.BLoadLayout(url, false, false)) {
        this.panorama.Msg(`[Modlock] ${mod} style sheet ${path} is unavailable`)
      }
      if (into !== root) {
        into.hittest = false
        this.write(into, 'width', '100%')
        this.write(into, 'height', '100%')
      }
    }
    return nested
  }

  /**
   * dressPlaceholder draws an entry's placeholder in the entry's color and
   * size, faded, as the stock styles would.
   */
  private dressPlaceholder(panel: Panel, style: Record<string, string>): void {
    // An entry the game made without a placeholder has nothing to dress.
    const placeholder = panel.FindChildTraverse('PlaceholderText')
    if (!placeholder) {
      return
    }

    // Fade it under the entry's own text, centered on its line.
    this.write(placeholder, 'color', style.color ?? entryLook.color!)
    this.write(placeholder, 'fontSize', style.fontSize ?? entryLook.fontSize!)
    this.write(placeholder, 'opacity', '0.5')
    this.write(placeholder, 'verticalAlign', 'center')
    this.write(placeholder, 'horizontalAlign', 'left')
  }

  /** write sets one style property; a refused property leaves the rest drawn. */
  private write(panel: Panel, name: string, value: string): void {
    this.attempt(`set the style ${name}`, () => {
      panel.style[name] = value
    })
  }

  /**
   * attempt returns what fn returns, or undefined when the game refuses it,
   * logging each refused kind of call once, so one refused call leaves the
   * rest of the screen drawn.
   */
  private attempt<T>(what: string, fn: () => T): T | undefined {
    try {
      return fn()
    } catch (error) {
      if (!refused.has(what)) {
        refused.add(what)
        this.panorama.Msg(
          `[Modlock] the game refused to ${what}: ${String(error)}`,
        )
      }
      return undefined
    }
  }
}

/** aligns maps each alignment to its horizontal and vertical style values. */
const aligns: Record<Align, [string, string]> = {
  [Align.START]: ['left', 'top'],
  [Align.CENTER]: ['center', 'center'],
  [Align.END]: ['right', 'bottom'],
}

/**
 * faces maps each typeface to the game's font stack, as its base styles name
 * them: Retail Demo for text, VALVEPulp for the clock and titles, Reaver for
 * menus.
 */
const faces: Partial<Record<Face, string>> = {
  [Face.BODY]: '"Retail Demo", "Noto Sans", sans-serif',
  [Face.BLOCK]: 'VALVEPulp, "Noto Sans", sans-serif',
  [Face.SERIF]: 'Reaver, serif',
}

/**
 * textShadow is a soft drop under the glyphs rather than a halo that thickens
 * them, so text reads over a bright sky.
 */
const textShadow = '0px 1px 3px 1 #000000d0'

/** placed returns the style properties that fill region, in screen percents. */
function placed(region: Region): Record<string, string> {
  const percent = (value: number | undefined) =>
    `${clamp((value ?? 0) * 100, 0, 100)}%`
  return {
    x: percent(region.left),
    y: percent(region.top),
    width: percent(region.width),
    height: percent(region.height),
  }
}

/** styleOf returns the style properties to write; a root fills the screen by default. */
function styleOf(
  style: Style | undefined,
  root: boolean,
  parentFlow: Flow | undefined,
): Record<string, string> {
  const along = flowAxes(parentFlow)
  const css: Record<string, string | null> = {
    width: length(style?.width, along.width) ?? (root ? '100%' : null),
    height: length(style?.height, along.height) ?? (root ? '100%' : null),
    minWidth: length(style?.minWidth, false),
    maxWidth: length(style?.maxWidth, false),
    minHeight: length(style?.minHeight, false),
    maxHeight: length(style?.maxHeight, false),
    flowChildren: style?.flow ? (flows[style.flow] ?? null) : null,
    horizontalAlign: style?.horizontalAlign
      ? aligns[style.horizontalAlign][0]
      : null,
    verticalAlign: style?.verticalAlign ? aligns[style.verticalAlign][1] : null,
    x: length(style?.x, false),
    y: length(style?.y, false),
    zIndex: style?.zIndex ? String(Math.trunc(finite(style.zIndex))) : null,
    overflow:
      style?.overflowX || style?.overflowY
        ? `${overflows[style.overflowX ?? Overflow.DEFAULT]} ${overflows[style.overflowY ?? Overflow.DEFAULT]}`
        : null,
    margin: edges(style?.margin),
    padding: edges(style?.padding),
    backgroundColor: style?.gradient
      ? gradient(style.gradient)
      : style?.background
        ? color(style.background)
        : null,
    boxShadow: style?.boxShadow ? boxShadow(style.boxShadow) : null,
    color: style?.color ? color(style.color) : null,
    fontSize: style?.fontSize ? `${finite(style.fontSize)}px` : null,
    fontWeight: style?.weight
      ? (weights[style.weight] ?? null)
      : style?.bold
        ? 'bold'
        : null,
    fontStyle: style?.italic ? 'italic' : null,
    lineHeight: style?.lineHeight ? `${finite(style.lineHeight)}px` : null,
    whiteSpace: style?.noWrap ? 'nowrap' : null,
    textOverflow: style?.textOverflow
      ? (textOverflows[style.textOverflow] ?? null)
      : null,
    textDecoration: style?.decoration
      ? (decorations[style.decoration] ?? null)
      : null,
    textAlign: style?.textAlign ? aligns[style.textAlign][0] : null,
    borderRadius: style?.borderRadius
      ? `${finite(style.borderRadius)}px`
      : null,
    opacity:
      style?.opacity === undefined ? null : String(clamp(style.opacity, 0, 1)),
    fontFamily: style?.face ? (faces[style.face] ?? null) : null,
    textTransform: style?.uppercase ? 'uppercase' : null,
    letterSpacing: style?.letterSpacing
      ? `${finite(style.letterSpacing)}px`
      : null,
    textShadow: style?.textShadow
      ? textShadowOf(style.textShadow)
      : style?.shadow
        ? textShadow
        : null,
    border:
      style?.borderWidth && style.borderColor
        ? `${finite(style.borderWidth)}px solid ${color(style.borderColor)}`
        : null,
    washColor: style?.tint ? color(style.tint) : null,
    saturation:
      style?.saturation === undefined
        ? null
        : String(clamp(style.saturation, 0, 10)),
    brightness:
      style?.brightness === undefined
        ? null
        : String(clamp(style.brightness, 0, 10)),
    contrast:
      style?.contrast === undefined
        ? null
        : String(clamp(style.contrast, 0, 10)),
    blur: style?.blur ? `gaussian(${clamp(style.blur, 0, 100)}px)` : null,
    transform: style?.transform ? transform(style.transform) : null,
    transformOrigin:
      style?.transform?.originX !== undefined ||
      style?.transform?.originY !== undefined
        ? `${finite(style.transform.originX ?? 50)}% ${finite(style.transform.originY ?? 50)}%`
        : null,
    preTransformRotate2d:
      style?.turn === undefined ? null : `${finite(style.turn)}deg`,
    clip: style?.clip ? clipOf(style.clip) : null,
  }
  return Object.fromEntries(
    Object.entries(css).filter(
      (entry): entry is [string, string] => entry[1] !== null,
    ),
  )
}

/**
 * layerOf returns the properties of a style laid over style, such as the
 * hover's: those that differ from style and that style either sets or can
 * return from, since the layer lifts again without a new panel.
 */
function layerOf(
  layer: Style | undefined,
  style: Record<string, string>,
  parentFlow: Flow | undefined,
): Record<string, string> {
  if (!layer) {
    return {}
  }
  return Object.fromEntries(
    Object.entries(styleOf(layer, false, parentFlow)).filter(
      ([name, value]) =>
        style[name] !== value && (name in style || name in rest),
    ),
  )
}

/**
 * cyclingOf returns the cycle a node runs, with its far end laid over style,
 * or null for none. A cycle swings at most twenty times a second.
 */
function cyclingOf(
  node: Node,
  style: Record<string, string>,
  parentFlow: Flow | undefined,
): Cycling | null {
  const cycle = node.cycle
  const far = layerOf(cycle?.style, style, parentFlow)
  if (!cycle || Object.keys(far).length === 0) {
    return null
  }
  return {
    key: JSON.stringify(cycle),
    style: far,
    seconds: clamp(cycle.seconds ?? 0, 0.05, 60),
    count: Math.trunc(clamp(cycle.count ?? 0, 0, 1000)),
  }
}

/** cycleTransition eases a cycle each way when its node sets no transition. */
function cycleTransition(cycle: Cycling | null): Transition | undefined {
  return cycle
    ? { seconds: cycle.seconds, easing: Easing.EASE_IN_OUT }
    : undefined
}

/**
 * transitionOf returns the properties that animate the changes between style
 * and the layers over it: every property any of them writes that the game
 * animates.
 */
function transitionOf(
  transition: Transition | undefined,
  style: Record<string, string>,
  layers: Record<string, string>,
): Record<string, string> {
  // An element without a duration does not animate.
  if (!transition?.seconds) {
    return {}
  }

  // Animate each property style or a layer writes that the game can animate.
  const names = [...new Set([...Object.keys(style), ...Object.keys(layers)])]
    .filter((name) => animated.has(name))
    .map((name) => name.replace(/[A-Z]/g, (cap) => `-${cap.toLowerCase()}`))
  if (names.length === 0) {
    return {}
  }
  return {
    transitionProperty: names.join(', '),
    transitionDuration: `${clamp(transition.seconds, 0, 60)}s`,
    transitionDelay: `${clamp(transition.delay ?? 0, 0, 60)}s`,
    transitionTimingFunction: transition.curve
      ? curveOf(transition.curve)
      : easings[transition.easing ?? Easing.EASE_OUT],
  }
}

/**
 * curveOf writes a cubic Bezier timing function. Its control points' times
 * stay within the transition, as the game requires; their progress may
 * overshoot it, to spring past the end and settle back.
 */
function curveOf(curve: Curve): string {
  const time = (value: number | undefined) => clamp(value ?? 0, 0, 1)
  const progress = (value: number | undefined) => clamp(value ?? 0, -10, 10)
  return `cubic-bezier( ${time(curve.x1)}, ${progress(curve.y1)}, ${time(curve.x2)}, ${progress(curve.y2)} )`
}

/** animated holds the style properties the game animates in a transition. */
const animated = new Set([
  'width',
  'height',
  'x',
  'y',
  'margin',
  'padding',
  'backgroundColor',
  'boxShadow',
  'color',
  'fontSize',
  'borderRadius',
  'opacity',
  'letterSpacing',
  'textShadow',
  'border',
  'washColor',
  'saturation',
  'brightness',
  'contrast',
  'blur',
  'transform',
  'transformOrigin',
  'preTransformRotate2d',
])

/**
 * rest maps each style property a node may drop to the value it rests at, as
 * a stock panel holds it before any style is written. A property with no
 * resting value, such as a font, makes the node a new panel when dropped.
 */
const rest: Record<string, string> = {
  width: 'fit-children',
  height: 'fit-children',
  flowChildren: 'none',
  horizontalAlign: 'left',
  verticalAlign: 'top',
  x: '0px',
  y: '0px',
  zIndex: '0',
  overflow: 'squish squish',
  margin: '0px 0px 0px 0px',
  padding: '0px 0px 0px 0px',
  backgroundColor: 'none',
  boxShadow: 'none',
  fontStyle: 'normal',
  whiteSpace: 'normal',
  textDecoration: 'none',
  textAlign: 'left',
  borderRadius: '0px',
  opacity: '1',
  textTransform: 'none',
  letterSpacing: '0px',
  textShadow: 'none',
  border: '0px none',
  washColor: 'none',
  saturation: '1',
  brightness: '1',
  contrast: '1',
  blur: 'gaussian(0px)',
  // A transform rests at the identity in the form styles write, so the game
  // eases a transform back to rest as it eases between two.
  transform: transform({}),
  transformOrigin: '50% 50%',
  preTransformRotate2d: '0deg',
  transitionProperty: 'none',
  transitionDuration: '0s',
  transitionDelay: '0s',
  transitionTimingFunction: 'ease',
}

/** flows maps each flow to the game's flow-children value. */
const flows: Partial<Record<Flow, string>> = {
  [Flow.DOWN]: 'down',
  [Flow.RIGHT]: 'right',
  [Flow.DOWN_WRAP]: 'down-wrap',
  [Flow.RIGHT_WRAP]: 'right-wrap',
}

/** overflows maps each overflow to the game's value for one direction. */
const overflows: Record<Overflow, string> = {
  [Overflow.DEFAULT]: 'squish',
  [Overflow.SQUISH]: 'squish',
  [Overflow.CLIP]: 'clip',
  [Overflow.SCROLL]: 'scroll',
  [Overflow.SHOW]: 'noclip',
}

/** weights maps each text weight to the game's font-weight value. */
const weights: Partial<Record<Weight, string>> = {
  [Weight.THIN]: 'thin',
  [Weight.LIGHT]: 'light',
  [Weight.NORMAL]: 'normal',
  [Weight.MEDIUM]: 'medium',
  [Weight.SEMI_BOLD]: 'semi-bold',
  [Weight.BOLD]: 'bold',
  [Weight.BLACK]: 'black',
}

/** textOverflows maps each text overflow to the game's value. */
const textOverflows: Partial<Record<TextOverflow, string>> = {
  [TextOverflow.ELLIPSIS]: 'ellipsis',
  [TextOverflow.SHRINK]: 'shrink',
  [TextOverflow.CLIP]: 'clip',
}

/** decorations maps each text decoration to the game's value. */
const decorations: Partial<Record<Decoration, string>> = {
  [Decoration.UNDERLINE]: 'underline',
  [Decoration.STRIKE]: 'line-through',
}

/** easings maps each easing to the game's timing function. */
const easings: Record<Easing, string> = {
  [Easing.EASE_OUT]: 'ease-out',
  [Easing.EASE_IN]: 'ease-in',
  [Easing.EASE_IN_OUT]: 'ease-in-out',
  [Easing.LINEAR]: 'linear',
}

/** scalings maps each way to fit a picture to the game's scaling value. */
const scalings: Partial<Record<Scaling, string>> = {
  [Scaling.FIT]: 'stretch-to-fit-preserve-aspect',
  [Scaling.COVER]: 'stretch-to-cover-preserve-aspect',
  [Scaling.STRETCH]: 'stretch',
  [Scaling.FIT_WIDTH]: 'stretch-to-fit-x-preserve-aspect',
  [Scaling.FIT_HEIGHT]: 'stretch-to-fit-y-preserve-aspect',
  [Scaling.NONE]: 'none',
}

/** looks maps each picture of a hero to the game's hero image style. */
const looks: Record<HeroLook, string> = {
  [HeroLook.SMALL]: 'small',
  [HeroLook.CARD]: 'hero_card',
  [HeroLook.ICON]: 'icon',
  [HeroLook.VERTICAL]: 'vertical',
  [HeroLook.TOP_BAR]: 'top_bar',
  [HeroLook.MINIMAP]: 'minimap_icon',
}

/**
 * madeWith returns the properties a node's panel is made with, which the game
 * reads only then: an image's scaling, a hero image's hero, picture and
 * scaling, an avatar's account, and an input's bounds and label. Other panels
 * take none.
 */
function madeWith(kind: Kind, node: Node): Record<string, string> | undefined {
  const scaling = node.scaling ? scalings[node.scaling] : undefined
  switch (kind) {
    case Kind.HERO_IMAGE:
      return {
        heroid: String(Math.trunc(finite(node.hero))),
        heroimagestyle: looks[node.heroLook ?? HeroLook.SMALL] ?? 'small',
        scaling: scaling ?? 'stretch-to-fit-preserve-aspect',
      }
    case Kind.AVATAR:
      // A Steam account's id is the low 32 bits of its Steam id.
      return { accountid: String((node.steamId ?? 0n) & 0xffffffffn) }
    case Kind.DOCUMENT_OUTPUT:
      return { allowtextselection: 'true', html: 'false' }
    case Kind.INPUT:
      return controlMadeWith(node)
    default:
      return kind === Kind.IMAGE && scaling ? { scaling } : undefined
  }
}

/**
 * controlMadeWith returns the properties an input's panel is made with: an
 * entry's limit and placeholder, a slider's range and step, and a toggle's
 * label.
 */
function controlMadeWith(node: Node): Record<string, string> | undefined {
  switch (node.input?.case) {
    case 'textEntry': {
      const entry = node.input.value
      const made: Record<string, string> = {
        maxchars: String(entry.maxLength || 100),
      }
      if (entry.placeholder) {
        made.placeholder = entry.placeholder
      }
      return made
    }
    case 'slider': {
      // Native sliders keep a normalized value. Convert both directions at
      // the renderer boundary rather than relying on creation attributes.
      const slider = node.input.value
      const [min, max] = sliderRange(slider)
      return {
        class: 'HorizontalSlider',
        direction: 'horizontal',
        min: '0',
        max: '1',
        increment: String(Math.max(0, finite(slider.step)) / (max - min)),
      }
    }
    case 'toggle':
      return { text: node.text ?? '' }
    default:
      return undefined
  }
}

/** showPlaceholder shows an entry's placeholder only while the entry is empty. */
function showPlaceholder(panel: Panel): void {
  const placeholder = panel.FindChildTraverse('PlaceholderText')
  if (placeholder) {
    placeholder.visible = !panel.text
  }
}

/** nodeInput returns an input's value as the mod drew it. */
function nodeInput(node: Node): InputValue | undefined {
  switch (node.input?.case) {
    case 'textEntry':
      return { case: 'text', value: node.input.value.text ?? '' }
    case 'slider':
      return { case: 'number', value: finite(node.input.value.value) }
    case 'toggle':
      return { case: 'on', value: !!node.input.value.on }
    case 'dropdown':
      return { case: 'choice', value: node.input.value.selected ?? 0 }
    default:
      return undefined
  }
}

/** readInput returns an input's value as its panel holds it. */
function readInput(
  control: Control | null,
  panel: Panel,
  slider?: Slider,
): InputValue | undefined {
  switch (control) {
    case 'textEntry':
      return { case: 'text', value: panel.text }
    case 'slider':
      return {
        case: 'number',
        value: Math.fround(sliderValue(panel.value, slider ?? {})),
      }
    case 'toggle':
      return { case: 'on', value: panel.checked }
    case 'dropdown': {
      // A dropdown's options are named by their index.
      const index = /^option(\d+)$/.exec(panel.GetSelected()?.id ?? '')?.[1]
      return index === undefined
        ? undefined
        : { case: 'choice', value: Number(index) }
    }
    default:
      return undefined
  }
}

/** sliderRange returns the mod's finite range, or the default 0 to 1. */
function sliderRange(slider: Slider): [number, number] {
  const min = finite(slider.min)
  const max = finite(slider.max)
  return max > min ? [min, max] : [0, 1]
}

/** sliderFraction maps the mod's value to the native slider's 0 to 1 range. */
function sliderFraction(slider: Slider): number {
  const [min, max] = sliderRange(slider)
  return clamp((finite(slider.value) - min) / (max - min), 0, 1)
}

/** sliderValue maps native position to the mod's range and step. */
function sliderValue(fraction: number, slider: Slider): number {
  const [min, max] = sliderRange(slider)
  const value = min + clamp(finite(fraction), 0, 1) * (max - min)
  const step = finite(slider.step)
  return step > 0
    ? clamp(min + Math.round((value - min) / step) * step, min, max)
    : value
}

/** soundOf returns a sound event's name, or nothing for a name the game cannot hold. */
function soundOf(sound: string | undefined): string {
  return sound && /^[\w.-]{1,128}$/.test(sound) ? sound : ''
}

/** ends maps each linear gradient's direction to its start and end points. */
const ends: Partial<Record<GradientShape, string>> = {
  [GradientShape.DOWN]: '0% 0%, 0% 100%',
  [GradientShape.RIGHT]: '0% 0%, 100% 0%',
  [GradientShape.UP]: '0% 100%, 0% 0%',
  [GradientShape.LEFT]: '100% 0%, 0% 0%',
}

/** gradient writes a gradient, or its one color, or null for no colors. */
function gradient(value: Gradient): string | null {
  // Fewer than two stops make no gradient.
  const stops = value.stops ?? []
  if (stops.length < 2) {
    return stops[0] ? color(stops[0].color ?? 0) : null
  }

  // Write the shape's ends, then each stop.
  const shape = value.shape ?? GradientShape.DOWN
  const head =
    shape === GradientShape.RADIAL
      ? 'radial, 50% 50%, 0% 0%, 50% 50%'
      : `linear, ${ends[shape] ?? ends[GradientShape.DOWN]}`
  const colors = stops.map(
    (stop) =>
      `color-stop( ${clamp(stop.at ?? 0, 0, 1)}, ${color(stop.color ?? 0)} )`,
  )
  return `gradient( ${head}, ${colors.join(', ')} )`
}

/** boxShadow writes a box's shadow. */
function boxShadow(value: Shadow): string {
  const inset = value.inset ? 'inset ' : ''
  return `${inset}${color(value.color ?? 0)} ${finite(value.x)}px ${finite(value.y)}px ${clamp(value.blur ?? 0, 0, 200)}px ${finite(value.spread)}px`
}

/** textShadowOf writes a text shadow, at full strength unless spread strengthens it. */
function textShadowOf(value: Shadow): string {
  const strength = value.spread ? clamp(value.spread, 0, 10) : 1
  return `${finite(value.x)}px ${finite(value.y)}px ${clamp(value.blur ?? 0, 0, 200)}px ${strength} ${color(value.color ?? 0)}`
}

/**
 * transform writes a move, a scale and a turn, always all three, so a
 * transition between two transforms matches each part with its own.
 */
function transform(value: Transform): string {
  const scaleX = value.scaleX ?? 1
  const scaleY = value.scaleY ?? scaleX
  return `translate3d( ${finite(value.translateX)}px, ${finite(value.translateY)}px, 0px ) scale3d( ${finite(scaleX)}, ${finite(scaleY)}, 1 ) rotateZ( ${finite(value.rotate)}deg )`
}

/**
 * clipOf writes the part of a panel that draws: a box whose sides lie at
 * percents of the panel's size from its top or left edge, or a slice of a
 * circle about a point, through sweep degrees from start.
 */
function clipOf(value: Clip): string | null {
  switch (value.shape?.case) {
    case 'rect': {
      const edge = (inset: number | undefined) =>
        `${clamp(inset ?? 0, 0, 100)}%`
      const box = value.shape.value
      return `rect( ${edge(box.top)}, ${edge(box.right)}, ${edge(box.bottom)}, ${edge(box.left)} )`
    }
    case 'radial': {
      const slice = value.shape.value
      return `radial( ${finite(slice.centerX ?? 50)}% ${finite(slice.centerY ?? 50)}%, ${finite(slice.start)}deg, ${clamp(slice.sweep ?? 0, 0, 360)}deg )`
    }
    default:
      return null
  }
}

/** refused holds the kinds of call the game refused, each logged once. */
const refused = new Set<string>()

/**
 * flowAxes returns the axes a parent's flow lays its children along: the game
 * honours fill-parent-flow only along them.
 */
function flowAxes(flow: Flow | undefined): { width: boolean; height: boolean } {
  return {
    width: flow === Flow.RIGHT || flow === Flow.RIGHT_WRAP,
    height: flow === Flow.DOWN || flow === Flow.DOWN_WRAP,
  }
}

/**
 * length writes a length, or null for none. A fill takes its share of the
 * parent's flow when flowing is true, and the whole parent otherwise.
 */
function length(value: Length | undefined, flowing: boolean): string | null {
  switch (value?.value?.case) {
    case 'pixels':
      return `${finite(value.value.value)}px`
    case 'percent':
      return `${clamp(value.value.value, 0, 100)}%`
    case 'fitChildren':
      return 'fit-children'
    case 'fill':
      return flowing
        ? `fill-parent-flow(${clamp(value.value.value, 0, 100)})`
        : '100%'
    default:
      return null
  }
}

/** edges writes four distances, or null for none. */
function edges(value: Edges | undefined): string | null {
  if (!value) {
    return null
  }
  return `${finite(value.top)}px ${finite(value.right)}px ${finite(value.bottom)}px ${finite(value.left)}px`
}

/** color writes 0xRRGGBBAA as rgba(). */
function color(value: number): string {
  return `rgba(${(value >>> 24) & 255}, ${(value >>> 16) & 255}, ${(value >>> 8) & 255}, ${((value & 255) / 255).toFixed(3)})`
}

/** clamp keeps value within [low, high]. */
function clamp(value: number, low: number, high: number): number {
  return Math.min(high, Math.max(low, finite(value)))
}

/** finite returns value, or zero when it is unset or not a finite number. */
function finite(value: number | undefined): number {
  return value !== undefined && Number.isFinite(value) ? value : 0
}
