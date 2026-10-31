// A mod shows each player an interface written with JSX:
//
//   import { show } from 'modlock'
//
//   show(player, (
//     <panel style={{ flow: 'down', horizontalAlign: 'center', margin: [80, 0, 0, 0] }}>
//       <label style={{ fontSize: 32, bold: true }}>Round {round}</label>
//       <button onPress={(player) => ready(player)}>Ready</button>
//     </panel>
//   ))
//
// The player's game draws each element with a stock panel, so no mod code
// runs there. show sends only the elements that changed since the last call
// for that player, so a mod may call it whenever its state changes, or every
// frame.
//
// Every element takes a style and a hover style, which the game draws while
// the pointer is over it and animates to when the style has a transition. A
// tooltip and a hover sound show and play on the same hover; a button plays
// its sound when pressed. A hero element shows one of the game's pictures of
// a hero, such as <hero hero={player.hero} look="small" />.
//
// An element's enter style is where its panel starts when it first shows,
// and its exit style where it goes as it leaves; its transition eases each
// way, so <panel style={{ opacity: 1, transition: { seconds: 0.2 } }}
// enter={{ opacity: 0 }}> fades in. A cycle swings an element to another style
// and back while it shows, as a pulse or a blink.
//
// Inputs take a value from the player and call onChange with the player and
// the value the host checked: <entry> a line of text, <slider> a number in its
// range, <toggle> on or off, and <dropdown> the index of an option. The value
// the mod draws is the truth; the player's edit shows until the mod's next
// value for that input differs from its last.
//
// A label with a token shows the game's own text in the player's language,
// such as <label token="#Citadel_HeroName_Haze" />, and an avatar shows a
// player's Steam picture: <avatar steamId={info.steamId} />.
//
// A layout element loads a layout from the mod's content addon, such as
// <layout src="shop.xml" data={stock} />. Its script runs in the player's
// game, so only a host that has reviewed the mod's content loads it; elsewhere
// it draws nothing. The script includes the Panorama library
// scripts/modlock/layout.js and reads data with Modlock.data.
//
// route sends a player a named JSON value that every layout of the mod reads
// with Modlock.route, wherever it sits in the tree, such as
// route(player, 'scores', { red: 3, blue: 1 }). The value stays until the
// next call; undefined removes it.
//
// toast shows a player a short notice at the lower left, above the game's
// health and stats, which fades after a few seconds; a mod answers a command
// or refuses one with it.
//
// hideHud hides parts of the game's own HUD from a player, such as their
// health bar or the notice for each boon, until the mod lists them no more,
// hides the player's interface, or stops.
//
// sheet adds a style sheet from the mod's content addon to every interface
// the mod shows, such as sheet('hud.css') for content/styles/hud.css, and an
// element's class names the sheet's classes:
//
//   <label class={leading ? 'Score Leading' : 'Score'}>{score}</label>
//
// A sheet styles only the mod's own panels, a later sheet wins over an
// earlier one, and an element's style wins over both. A class change runs the
// sheet's transitions. Sheets load where layouts do; elsewhere the elements
// draw with their own styles.

import { itemId } from './item-id.js'
import {
  Align,
  type Change,
  type Cycle as WireCycle,
  Decoration,
  Easing,
  Face,
  Flow,
  GradientShape,
  HeroLook,
  HudPart as WireHudPart,
  Kind,
  Overflow as WireOverflow,
  Scaling as WireScaling,
  TextOverflow,
  type Edges as WireEdges,
  type Gradient as WireGradient,
  type Length as WireLength,
  type Node,
  type Shadow as WireShadow,
  type Style as WireStyle,
  type Transform as WireTransform,
  type Transition as WireTransition,
  UiRequest,
  Weight,
} from '../../src/wasm/ui/ui.pb.js'
import type { Player } from './host.gen.js'
import { call } from './host.js'

/**
 * Length is a size: pixels, a percentage of the parent, 'fit' to size to the
 * children, or 'fill' to share the space left in the parent's flow.
 */
export type Length = number | `${number}%` | 'fit' | 'fill'

/**
 * Edges are distances in pixels: one for every side, [vertical, horizontal],
 * or [top, right, bottom, left]. They are an array rather than a union of
 * tuples, which scriptc does not compile.
 */
export type Edges = number | readonly number[]

/**
 * Overflow decides what happens to children that do not fit: 'squish' shrinks
 * them, 'clip' cuts them off, 'scroll' lets the player scroll to them, and
 * 'show' draws them outside the element.
 */
export type Overflow = 'squish' | 'clip' | 'scroll' | 'show'

/**
 * Gradient blends colors across an element in place of its background. The
 * colors run down by default; at places each color from 0 to 1, spaced evenly
 * when unset.
 */
export interface Gradient {
  shape?: 'down' | 'right' | 'up' | 'left' | 'radial'
  colors: readonly string[]
  at?: readonly number[]
}

/**
 * Shadow is a soft shadow, offset by x and y and softened by blur, in pixels.
 * Spread grows a box's shadow in pixels, or strengthens a text shadow, where 0
 * draws it at full strength; inset draws a box's shadow inside its edge.
 */
export interface Shadow {
  x?: number
  y?: number
  blur?: number
  spread?: number
  color: string
  inset?: boolean
}

/**
 * Transition animates each change of an element's style, and the move to its
 * hover style and back, over seconds after delay. Easing is 'out' unless set.
 */
export interface Transition {
  seconds: number
  delay?: number
  easing?: 'out' | 'in' | 'inOut' | 'linear'
}

/**
 * Style lays out and paints an element. Colors are '#rrggbb' or '#rrggbbaa'.
 * Options that take one or two numbers are an array rather than a union of
 * tuples, which scriptc does not compile.
 */
export interface Style {
  width?: Length
  height?: Length
  /** minWidth, maxWidth, minHeight and maxHeight bound the element's size. */
  minWidth?: Length
  maxWidth?: Length
  minHeight?: Length
  maxHeight?: Length
  /**
   * flow places the children in a column or a row, or in columns or rows that
   * wrap where one is full; unset stacks them.
   */
  flow?: 'down' | 'right' | 'downWrap' | 'rightWrap'
  /**
   * x and y place the element at an offset from its parent's top left corner,
   * in pixels or a percent of the parent, outside the parent's flow.
   */
  x?: number | `${number}%`
  y?: number | `${number}%`
  /** zIndex draws the element above its siblings with a lower index. */
  zIndex?: number
  /** overflow decides what children that do not fit do, across and down. */
  overflow?: Overflow
  /** overflowX and overflowY set overflow for one direction. */
  overflowX?: Overflow
  overflowY?: Overflow
  horizontalAlign?: 'left' | 'center' | 'right'
  verticalAlign?: 'top' | 'center' | 'bottom'
  margin?: Edges
  padding?: Edges
  background?: string
  color?: string
  fontSize?: number
  bold?: boolean
  textAlign?: 'left' | 'center' | 'right'
  borderRadius?: number
  /** opacity fades the element and its children, from 0 to 1. */
  opacity?: number
  /**
   * face is one of the game's typefaces: 'body' its text, 'block' the
   * capitals of its clock and titles, 'serif' its menus.
   */
  face?: 'body' | 'block' | 'serif'
  /** uppercase sets the text in capitals. */
  uppercase?: boolean
  /** letterSpacing widens the space between letters, in pixels. */
  letterSpacing?: number
  /** shadow draws a soft drop shadow under the text, to read over the world. */
  shadow?: boolean
  /** textShadow draws this shadow under the text in place of shadow's. */
  textShadow?: Shadow
  /**
   * borderWidth and borderColor draw a line around the element's edge; it
   * shows only when both are set.
   */
  borderWidth?: number
  borderColor?: string
  /** gradient fills the element in place of its background. */
  gradient?: Gradient
  /** boxShadow draws a shadow around or inside the element's box. */
  boxShadow?: Shadow
  /** tint washes the element and its children with a color, recoloring a white icon. */
  tint?: string
  /** saturation, brightness and contrast scale the colors; 1 leaves them. */
  saturation?: number
  brightness?: number
  contrast?: number
  /** blur blurs the element and its children by a radius in pixels. */
  blur?: number
  italic?: boolean
  /** weight sets the text's weight in place of bold. */
  weight?:
    | 'thin'
    | 'light'
    | 'normal'
    | 'medium'
    | 'semiBold'
    | 'bold'
    | 'black'
  /** lineHeight is the height of each line of text, in pixels. */
  lineHeight?: number
  /** noWrap keeps the text on one line. */
  noWrap?: boolean
  /**
   * textOverflow ends text that does not fit with an 'ellipsis', 'shrink's it
   * until it fits, or 'clip's it at the edge.
   */
  textOverflow?: 'ellipsis' | 'shrink' | 'clip'
  /** decoration draws a line under or through the text. */
  decoration?: 'underline' | 'strike'
  /** translate moves the element as drawn by [x, y] pixels. */
  translate?: readonly number[]
  /** scale scales the element as drawn: one factor, or [x, y]. */
  scale?: number | readonly number[]
  /** rotate turns the element clockwise, in degrees. */
  rotate?: number
  /**
   * origin is the point the element scales and turns about, as [x, y]
   * percents of its size; unset is its center.
   */
  origin?: readonly number[]
  /**
   * transition animates each change of the style: seconds, or a Transition.
   * Animate transform, opacity and colors; size and placement animate too, but
   * move the element's neighbors each frame.
   */
  transition?: number | Transition
}

/**
 * HudPart is a part of the game's own HUD: 'levelUp' the notice for each boon,
 * ability point and power increase, 'ownHealth' the player's health bar with
 * its shields and regeneration, 'topBar' the bar of team scores, portraits
 * and clock across the top, 'ammo' the clip ring, count and ammo warnings at
 * the crosshair, 'souls' the souls and level readout, 'minimap' the minimap
 * with its hints and district name, 'abilities' the ability and active item
 * bar. Whatever a part does not name stays, such as the crosshair and chat.
 */
export type HudPart =
  | 'levelUp'
  | 'ownHealth'
  | 'topBar'
  | 'ammo'
  | 'souls'
  | 'minimap'
  | 'abilities'
  | 'status'

/** Child is anything an element may hold. Text becomes a label. */
export type Child =
  | UiElement
  | string
  | number
  | boolean
  | null
  | undefined
  | readonly Child[]

/** Component returns the elements that stand for its props. */
export type Component<P> = (props: P) => Child

/**
 * UiElement is one element of an interface, as JSX builds it. A component's
 * element is a fragment holding what the component returned.
 */
export interface UiElement {
  readonly type: string
  readonly props: Props
  readonly key?: string
}

/**
 * Props are an element's props: every intrinsic element's, by name, so the
 * encoder reads them as typed fields. A component's props pass through to it.
 */
export interface Props {
  readonly class?: string
  readonly style?: Style
  readonly hover?: Style
  readonly tooltip?: string
  readonly item?: string
  readonly hoverSound?: string
  readonly children?: Child
  readonly src?: string
  readonly scaling?: Scaling
  readonly hero?: number
  readonly look?: HeroLookName
  readonly data?: unknown
  readonly sound?: string
  readonly disabled?: boolean
  readonly onPress?: (player: Player) => void
  readonly enter?: Style
  readonly exit?: Style
  readonly cycle?: Cycle
  readonly token?: string
  readonly variables?: Readonly<Record<string, string | number>>
  readonly steamId?: bigint
  readonly value?: string | number
  readonly placeholder?: string
  readonly maxLength?: number
  readonly min?: number
  readonly max?: number
  readonly step?: number
  readonly on?: boolean
  readonly options?: readonly string[]
  readonly selected?: number
  readonly onChange?: (player: Player, value: never) => void
}

/**
 * ElementProps are the props every drawn element takes. hover is the style
 * drawn over style while the pointer is over the element; tooltip is the text
 * the game's tooltip shows then, item names an item by class name, such as
 * 'upgrade_ability_power_shard', whose shop card shows instead, and
 * hoverSound is the game's sound event played as the pointer enters, such as
 * 'UI.MainMenu.Hover'. An element with any of them takes the pointer, so the
 * game behind it does not.
 */
export interface ElementProps {
  /** class names the element's classes in the mod's sheets, separated by spaces. */
  class?: string
  style?: Style
  hover?: Style
  tooltip?: string
  item?: string
  hoverSound?: string
  /** enter is the style the element's panel starts from when it first shows. */
  enter?: Style
  /** exit is the style the element's panel eases to as it leaves. */
  exit?: Style
  /** cycle swings the element to another style and back while it shows. */
  cycle?: Cycle
}

/**
 * Cycle swings an element from its style to style and back, each way taking
 * seconds, count times or, without a count, while it shows.
 */
export interface Cycle {
  style: Style
  seconds: number
  count?: number
}

/** PanelProps lay out a panel, which groups its children. */
export interface PanelProps extends ElementProps {
  children?: Child
}

/**
 * LabelProps show text: the label's text children, joined, or the game's text
 * for token in the player's language, with each {s:name} place in it filled
 * from variables.
 */
export interface LabelProps extends ElementProps {
  children?: Child
  token?: string
  variables?: Readonly<Record<string, string | number>>
}

/** AvatarProps show the Steam picture of the player with steamId. */
export interface AvatarProps extends ElementProps {
  steamId: bigint
}

/**
 * InputProps are the props every input takes. A disabled input takes no
 * changes.
 */
export interface InputProps extends ElementProps {
  disabled?: boolean
}

/**
 * EntryProps make a box the player types a line into: value is its text,
 * placeholder the faint text it shows empty, and maxLength its limit in
 * characters, 100 unless set, and at most 240 bytes of text.
 */
export interface EntryProps extends InputProps {
  value?: string
  placeholder?: string
  maxLength?: number
  onChange?: (player: Player, text: string) => void
}

/**
 * SliderProps make a bar the player drags to a number from min to max, 0 to 1
 * unless set, stopping at each step when step is set.
 */
export interface SliderProps extends InputProps {
  value?: number
  min?: number
  max?: number
  step?: number
  onChange?: (player: Player, value: number) => void
}

/** ToggleProps make a box the player checks, labeled by its text children. */
export interface ToggleProps extends InputProps {
  children?: Child
  on?: boolean
  onChange?: (player: Player, on: boolean) => void
}

/** DropdownProps let the player pick one of options; selected is its index. */
export interface DropdownProps extends InputProps {
  options: readonly string[]
  selected?: number
  onChange?: (player: Player, index: number) => void
}

/**
 * Scaling fits a picture into its element's box: 'fit' shows it whole, 'cover'
 * fills the box and crops, 'stretch' fills the box and stretches, 'fitWidth'
 * and 'fitHeight' fit one side, and 'none' draws it at its own size.
 */
export type Scaling =
  | 'fit'
  | 'cover'
  | 'stretch'
  | 'fitWidth'
  | 'fitHeight'
  | 'none'

/** ImageProps show an image from the game's content, such as 'file://{images}/hud/icon.png'. */
export interface ImageProps extends ElementProps {
  src: string
  scaling?: Scaling
}

/**
 * HeroLookName is one of the game's pictures of a hero: 'small' the square
 * portrait of the kill feed, 'card' the hero picker's tall card, 'icon' the
 * round icon, 'vertical' the hero grid's narrow portrait, 'topBar' the top
 * bar's portrait and 'minimap' the minimap icon.
 */
export type HeroLookName =
  | 'small'
  | 'card'
  | 'icon'
  | 'vertical'
  | 'topBar'
  | 'minimap'

/** HeroProps show a picture of hero, by the game's hero id. */
export interface HeroProps extends ElementProps {
  hero: number
  look?: HeroLookName
  scaling?: Scaling
}

/**
 * ButtonProps make a button, which holds its children and calls onPress
 * with the player who pressed it. Players press buttons while they have a
 * cursor. sound is the game's sound event the press plays, such as
 * 'UI.MainMenu.Activate', and a disabled button takes no presses.
 */
export interface ButtonProps extends ElementProps {
  children?: Child
  sound?: string
  disabled?: boolean
  onPress?: (player: Player) => void
}

/**
 * LayoutProps load a layout from the mod's content addon: src is its path
 * there, such as 'shop.xml', and data is the JSON value its script reads with
 * Modlock.data.
 */
export interface LayoutProps {
  class?: string
  style?: Style
  src: string
  data?: unknown
}

/** JSX types the elements a mod's JSX may use. */
export declare namespace JSX {
  type Element = UiElement
  // The compiler applies IntrinsicAttributes only to components, so each
  // element takes a key as well.
  interface IntrinsicElements {
    panel: PanelProps & IntrinsicAttributes
    label: LabelProps & IntrinsicAttributes
    image: ImageProps & IntrinsicAttributes
    hero: HeroProps & IntrinsicAttributes
    button: ButtonProps & IntrinsicAttributes
    layout: LayoutProps & IntrinsicAttributes
    avatar: AvatarProps & IntrinsicAttributes
    entry: EntryProps & IntrinsicAttributes
    slider: SliderProps & IntrinsicAttributes
    toggle: ToggleProps & IntrinsicAttributes
    dropdown: DropdownProps & IntrinsicAttributes
  }
  interface IntrinsicAttributes {
    key?: string | number
  }
  interface ElementChildrenAttribute {
    children: unknown
  }
}

/**
 * jsx builds one element, rendering a component in place; the TypeScript
 * compiler calls it for JSX.
 */
export function jsx(
  type: string | Component<Props>,
  props: Props,
  key?: string | number,
): UiElement {
  const named = key === undefined ? undefined : String(key)
  if (typeof type === 'function') {
    return { type: fragment, props: { children: type(props) }, key: named }
  }
  return { type, props, key: named }
}

/** fragment is the type of a component's element. */
const fragment = 'fragment'

/** jsxs builds an element with several children, as jsx does. */
export const jsxs = jsx

/** Fragment groups children without a panel of its own. */
export function Fragment(props: { children?: Child }): Child {
  return props.children
}

/**
 * show replaces the interface the mod shows player with root. A player who
 * left and a mod that restarted show nothing until the next call.
 */
export function show(player: Player, root: Child): boolean {
  views.set(player.slot, { ...view(player), root })
  return draw(player)
}

/**
 * hideHud hides parts of the game's own HUD from player until the next call;
 * an empty list shows them all again.
 */
export function hideHud(player: Player, parts: readonly HudPart[]): boolean {
  views.set(player.slot, { ...view(player), hud: parts })
  return draw(player)
}

/**
 * freeCursor shows player's mouse cursor without Tab held while free is true,
 * so they can click the mod's panels, such as a pick screen; false gives the
 * mouse back to aiming.
 */
export function freeCursor(player: Player, free: boolean): boolean {
  views.set(player.slot, { ...view(player), cursor: free })
  return draw(player)
}

/**
 * sheet adds the style sheet at path in the mod's content styles directory,
 * such as 'hud.css', to every interface the mod shows, after the sheets added
 * before it. Adding a sheet again changes nothing. A path is letters, digits,
 * '_', '-' and '.' in segments separated by '/', ending in '.css'; another
 * adds nothing and returns false. The build turns an import of a sheet, such
 * as import './content/styles/hud.css', into this call.
 */
export function sheet(path: string): boolean {
  // Refuse a path the renderer cannot load, and keep a sheet added before.
  if (
    !path.endsWith('.css') ||
    !path.split('/').every((part) => sheetPart.test(part) && part !== '..')
  ) {
    return false
  }
  if (sheets.includes(path)) {
    return true
  }

  // Redraw every interface with the new sheet.
  sheets.push(path)
  for (const current of views.values()) {
    draw(current.player)
  }
  return true
}

/** sheetPart matches one segment of a sheet's path. */
const sheetPart = /^[\w.-]+$/

/** sheets lists the mod's style sheets, in the order the mod added them. */
const sheets: string[] = []

/**
 * route sends player value under name, for every layout of the mod to read
 * with Modlock.route, until the next call; undefined removes it. A name is
 * letters, digits, '_', '-' and '.', and the value is anything JSON.stringify
 * takes. A route with another name, or a value JSON cannot hold, sends nothing
 * and returns false.
 */
export function route(player: Player, name: string, value: unknown): boolean {
  // Refuse a name no attribute can carry and a value JSON cannot hold.
  const routes = new Map(view(player).routes)
  const encoded = value === undefined ? undefined : JSON.stringify(value)
  if (!routeName.test(name) || (value !== undefined && encoded === undefined)) {
    return false
  }

  // Store the value, or forget the route, and redraw.
  if (encoded === undefined) {
    routes.delete(name)
  } else {
    routes.set(name, encoded)
  }
  views.set(player.slot, { ...view(player), routes })
  return draw(player)
}

/** routeName matches the name of a route, as the renderer accepts it. */
const routeName = /^[\w.-]+$/

/** hide removes the interface, the routes and the notices the mod shows player. */
export function hide(player: Player): boolean {
  views.delete(player.slot)
  screens.delete(player.slot)
  return send(player.slot, { reset: true })
}

/**
 * toast shows player text in a notice at the lower left for seconds.
 * Notices stack, newest last, slide in, and fade as they expire.
 */
export function toast(player: Player, text: string, seconds = 4): boolean {
  const current = view(player)
  const notice = { id: nextNotice++, text, until: now + seconds }
  views.set(player.slot, {
    ...current,
    notices: [...current.notices, notice].slice(-maxNotices),
  })
  return draw(player)
}

/** press runs the handler of the button the player pressed. */
export function press(player: Player, node: string): void {
  const pressed = screens.get(player.slot)?.presses.get(node)
  if (pressed) {
    pressed.handler(player)
  }
}

/**
 * change runs the handler of the input the player changed, with its value as
 * the host checked it.
 */
export function change(
  player: Player,
  node: string,
  value: string | number | boolean | undefined,
): void {
  const changed = screens.get(player.slot)?.changes.get(node)
  if (changed) {
    changed.handler(player, value as never)
  }
}

/** tick advances the clock to the frame at seconds and fades expiring notices. */
export function tick(seconds: number): void {
  now = seconds
  for (const current of views.values()) {
    if (current.notices.length === 0) {
      continue
    }
    current.notices = current.notices.filter((notice) => notice.until > now)
    draw(current.player)
  }
}

/** View is what the mod shows one player: its interface and its notices. */
interface View {
  readonly player: Player
  readonly root: Child
  readonly hud: readonly HudPart[]
  /** cursor is true while the mod frees the player's mouse cursor. */
  readonly cursor: boolean
  /** routes maps each route's name to its value as JSON. */
  readonly routes: ReadonlyMap<string, string>
  notices: readonly Notice[]
}

/** Notice is one toast, shown until the game clock passes until. */
interface Notice {
  readonly id: number
  readonly text: string
  readonly until: number
}

/** views holds what the mod shows each player, by slot. */
const views = new Map<number, View>()

/** now is the game clock at the last frame, in seconds. */
let now = 0

/** nextNotice numbers the next notice, which keys it in the stack. */
let nextNotice = 0

/** maxNotices bounds the stack; a newer notice pushes out the oldest. */
const maxNotices = 4

/** noticeMotion slides a notice in from the left and fades it out when it expires. */
const noticeMotion = {
  enter: { opacity: 0, translate: [-16, 0] },
  exit: { opacity: 0 },
  transition: { seconds: 0.25, easing: 'out' },
} as const

/** view returns what the mod shows player, or an empty view. */
function view(player: Player): View {
  return (
    views.get(player.slot) ?? {
      player,
      root: undefined,
      hud: [],
      cursor: false,
      routes: new Map(),
      notices: [],
    }
  )
}

/** draw sends what changed in player's interface and notices. */
function draw(player: Player): boolean {
  // Render the player's view and any notices against the last screen sent.
  const current = view(player)
  const root = [
    current.root,
    current.notices.length !== 0 && notices(current.notices),
  ]
  const previous = screens.get(player.slot)
  const hud = current.hud.map((part) => hudParts[part])
  const next = render(previous, root, hud, current.cursor, current.routes)
  const change = diff(previous, next)

  // Send the change; a failed send means the player may have left, so send
  // the screen whole.
  if (change && !send(player.slot, change)) {
    const whole = render(undefined, root, hud, current.cursor, current.routes)
    if (!send(player.slot, diff(undefined, whole)!)) {
      screens.delete(player.slot)
      return false
    }
    screens.set(player.slot, whole)
    return true
  }

  // Keep the screen the player now holds.
  screens.set(player.slot, next)
  return true
}

/**
 * notices stacks the notices at the lower left, newest at the bottom, clear of
 * the game's health, weapon and spirit stats and the status icons above them.
 */
function notices(stack: readonly Notice[]): Child {
  // The stack fades with its last notice.
  const style: Style = {
    width: 'fit',
    height: 'fit',
    flow: 'down',
    horizontalAlign: 'left',
    verticalAlign: 'bottom',
    margin: [0, 0, 260, 24],
    transition: noticeMotion.transition,
  }
  const labels = stack.map((notice) =>
    jsx(
      'label',
      {
        style: {
          horizontalAlign: 'left',
          margin: [4, 0, 0, 0],
          padding: [5, 10],
          background: '#101418d8',
          color: '#f2f2f2',
          fontSize: 16,
          borderRadius: 4,
          transition: noticeMotion.transition,
        },
        enter: noticeMotion.enter,
        exit: noticeMotion.exit,
        children: notice.text,
      },
      notice.id,
    ),
  )
  return jsx('panel', { style, exit: noticeMotion.exit, children: labels })
}

/** Screen is what one player's interface holds, as the host last accepted it. */
interface Screen {
  /** nodes maps each node's id to its encoding, to find what changed. */
  readonly nodes: Map<string, { node: Node; encoded: string }>
  /**
   * ids maps each element's place in the tree to its node's id. A place is
   * its parent's place, then "/" for the children, ",index" or ",=key" in an
   * array, and ">" for what a component returned.
   */
  readonly ids: Map<string, string>
  /**
   * presses maps each button's id to its handler, held in a record since
   * scriptc has no Map slot for a function.
   */
  readonly presses: Map<string, { readonly handler: (player: Player) => void }>
  /** changes maps each input's id to its handler, as presses does. */
  readonly changes: Map<
    string,
    { readonly handler: (player: Player, value: never) => void }
  >
  /** next numbers the next new node, so an id is never reused. */
  next: number
}

/** screens holds each player's interface by slot. */
const screens = new Map<number, Screen>()

/**
 * render lays root out as nodes, keeping the ids of elements that stayed in
 * place, beside a node for each route.
 */
function render(
  previous: Screen | undefined,
  root: Child,
  hud: readonly WireHudPart[],
  cursor: boolean,
  routes: ReadonlyMap<string, string>,
): Screen {
  // The root is the full-screen panel the elements hang from, and it lists
  // the parts of the game's HUD to hide, whether the cursor is free and the
  // mod's sheets.
  const layout = new Layout(previous)
  layout.add({
    id: '',
    kind: Kind.PANEL,
    children: place(layout, root, '/'),
    hideHud: [...hud],
    freeCursor: cursor,
    sheets: [...sheets],
  })

  // A route's id holds a colon, which no element's id does.
  for (const [name, data] of routes) {
    layout.add({ id: `route:${name}`, kind: Kind.ROUTE, route: name, data })
  }
  return layout.screen
}

/**
 * Layout holds one screen while place lays it out. The steps are not closures:
 * a closure that calls itself holds itself, and QuickJS frees such a cycle,
 * with the screens it reaches, only in a full collection that walks the mod's
 * whole heap in one frame. place is a function rather than a method because
 * scriptc cannot hold a method that takes a Child as a prototype value.
 */
class Layout {
  readonly screen: Screen
  private readonly previous: Screen | undefined

  /** The layout continues previous's id counter and keeps its ids. */
  constructor(previous: Screen | undefined) {
    this.previous = previous
    this.screen = {
      nodes: new Map(),
      ids: new Map(),
      presses: new Map(),
      changes: new Map(),
      next: previous?.next ?? 0,
    }
  }

  /** add records a node. */
  add(node: Node): void {
    // An avatar's Steam id is a bigint, which JSON writes as its digits here.
    const encoded = JSON.stringify(node, (_, value: unknown) =>
      typeof value === 'bigint' ? String(value) : value,
    )
    this.screen.nodes.set(node.id!, { node, encoded })
  }

  /** id returns the node id for the element at place. */
  id(place: string): string {
    let found = this.previous?.ids.get(place)
    if (found === undefined) {
      found = (this.screen.next++).toString(36)
    }
    this.screen.ids.set(place, found)
    return found
  }
}

/** place lays out child at place in layout and returns its nodes' ids. */
function place(layout: Layout, child: Child, at: string): string[] {
  // Lay out empty values, lists, text, and the fragments components return.
  if (child === null || child === undefined || typeof child === 'boolean') {
    return []
  }
  if (isList(child)) {
    return child.flatMap((item, index) => {
      const key = isElement(item) ? item.key : undefined
      return place(
        layout,
        item,
        key === undefined ? `${at},${index}` : `${at},=${key}`,
      )
    })
  }
  if (!isElement(child)) {
    const node: Node = {
      id: layout.id(at),
      kind: Kind.LABEL,
      text: String(child),
    }
    layout.add(node)
    return [node.id!]
  }
  if (child.type === fragment) {
    return place(layout, child.props.children, `${at}>`)
  }

  // Draw an intrinsic element.
  const props = child.props
  const node: Node = {
    id: layout.id(at),
    style: wireStyle(props.style),
    hover: wireStyle(props.hover),
    tooltip: props.tooltip,
    item: props.item ? itemId(props.item) : undefined,
    hoverSound: props.hoverSound,
    enter: wireStyle(props.enter),
    exit: wireStyle(props.exit),
    cycle: cycle(props.cycle),
    classes: classes(props.class),
  }
  switch (child.type) {
    case 'label':
      node.kind = Kind.LABEL
      node.text = text(props.children)
      node.token = props.token
      node.variables = variables(props.variables)
      break
    case 'avatar':
      node.kind = Kind.AVATAR
      node.steamId = props.steamId
      break
    case 'entry':
    case 'slider':
    case 'toggle':
    case 'dropdown':
      input(layout, node, child.type, props)
      break
    case 'image':
      node.kind = Kind.IMAGE
      node.image = props.src ?? ''
      node.scaling = scaling(props.scaling)
      break
    case 'hero':
      node.kind = Kind.HERO_IMAGE
      node.hero = props.hero
      node.heroLook = props.look === undefined ? undefined : looks[props.look]
      node.scaling = scaling(props.scaling)
      break
    case 'button':
      node.kind = Kind.BUTTON
      node.sound = props.sound
      node.disabled = props.disabled
      // A disabled button's press runs nothing, whoever sends it.
      if (props.onPress && !props.disabled) {
        layout.screen.presses.set(node.id!, { handler: props.onPress })
      }
      node.children = place(layout, props.children, `${at}/`)
      break
    case 'layout':
      node.kind = Kind.LAYOUT
      node.layout = props.src ?? ''
      node.data =
        props.data === undefined ? '' : (JSON.stringify(props.data) ?? '')
      break
    default:
      node.kind = Kind.PANEL
      node.children = place(layout, props.children, `${at}/`)
  }
  layout.add(node)
  return [node.id!]
}

/** classes splits an element's class into its names, or undefined for none. */
function classes(names: string | undefined): string[] | undefined {
  const list = (names ?? '').split(' ').filter((name) => name !== '')
  return list.length === 0 ? undefined : list
}

/** input lays out an input of kind from props in node. */
function input(
  layout: Layout,
  node: Node,
  kind: 'entry' | 'slider' | 'toggle' | 'dropdown',
  props: Props,
): void {
  // A disabled input's change runs nothing, whoever sends it.
  node.kind = Kind.INPUT
  node.disabled = props.disabled
  if (props.onChange && !props.disabled) {
    layout.screen.changes.set(node.id!, { handler: props.onChange })
  }

  // Each control carries its value and its bounds.
  switch (kind) {
    case 'entry':
      node.input = {
        case: 'textEntry',
        value: {
          text: String(props.value ?? ''),
          placeholder: props.placeholder,
          maxLength: props.maxLength,
        },
      }
      break
    case 'slider':
      node.input = {
        case: 'slider',
        value: {
          value: Number(props.value ?? 0),
          min: props.min,
          max: props.max,
          step: props.step,
        },
      }
      break
    case 'toggle':
      node.text = text(props.children)
      node.input = { case: 'toggle', value: { on: props.on } }
      break
    case 'dropdown':
      node.input = {
        case: 'dropdown',
        value: {
          options: [...(props.options ?? [])],
          selected: props.selected,
        },
      }
  }
}

/** variables encodes a localized label's variables as text. */
function variables(
  value: Readonly<Record<string, string | number>> | undefined,
): Record<string, string> | undefined {
  if (!value) {
    return undefined
  }
  const encoded: Record<string, string> = {}
  for (const [name, filled] of Object.entries(value)) {
    encoded[name] = String(filled)
  }
  return encoded
}

/** cycle encodes a cycle. */
function cycle(value: Cycle | undefined): WireCycle | undefined {
  if (!value) {
    return undefined
  }
  return {
    style: wireStyle(value.style),
    seconds: value.seconds,
    count: value.count,
  }
}

/** diff returns the change from previous to next, or undefined when nothing changed. */
function diff(previous: Screen | undefined, next: Screen): Change | undefined {
  const set = [...next.nodes.values()]
    .filter(
      ({ node, encoded }) => previous?.nodes.get(node.id!)?.encoded !== encoded,
    )
    .map(({ node }) => node)
  const removed = [...(previous?.nodes.keys() ?? [])].filter(
    (id) => !next.nodes.has(id),
  )
  if (previous && set.length === 0 && removed.length === 0) {
    return undefined
  }
  return { reset: !previous, set, removed }
}

/** send asks the host to apply change to the player in slot. */
function send(slot: number, change: Change): boolean {
  return call('Ui', UiRequest.toBinary({ player: slot, change })) !== undefined
}

/** isElement reports whether child is an element. */
function isElement(child: Child): child is UiElement {
  return typeof child === 'object' && child !== null && !Array.isArray(child)
}

/**
 * isList reports whether a child is a list of children. Array.isArray does not
 * narrow a readonly array type, so the list case needs its own guard.
 */
function isList(child: Child): child is readonly Child[] {
  return Array.isArray(child)
}

/** text joins a label's text children. */
function text(children: Child): string {
  if (
    children === null ||
    children === undefined ||
    typeof children === 'boolean'
  ) {
    return ''
  }
  if (isList(children)) {
    return children.map(text).join('')
  }
  return isElement(children) ? '' : String(children)
}

/** aligns maps each alignment to its wire value. */
const aligns = {
  left: Align.START,
  top: Align.START,
  center: Align.CENTER,
  right: Align.END,
  bottom: Align.END,
} as const

/** align encodes an alignment. */
function align(value: keyof typeof aligns | undefined): Align | undefined {
  return value === undefined ? undefined : aligns[value]
}

/** hudParts maps each part of the game's HUD to its wire value. */
const hudParts = {
  levelUp: WireHudPart.LEVEL_UP,
  ownHealth: WireHudPart.OWN_HEALTH,
  topBar: WireHudPart.TOP_BAR,
  ammo: WireHudPart.AMMO,
  souls: WireHudPart.SOULS,
  minimap: WireHudPart.MINIMAP,
  abilities: WireHudPart.ABILITIES,
  status: WireHudPart.STATUS,
} as const

/** faces maps each typeface to its wire value. */
const faces = {
  body: Face.BODY,
  block: Face.BLOCK,
  serif: Face.SERIF,
} as const

/** looks maps each picture of a hero to its wire value. */
const looks = {
  small: HeroLook.SMALL,
  card: HeroLook.CARD,
  icon: HeroLook.ICON,
  vertical: HeroLook.VERTICAL,
  topBar: HeroLook.TOP_BAR,
  minimap: HeroLook.MINIMAP,
} as const

/** scalings maps each way to fit a picture to its wire value. */
const scalings = {
  fit: WireScaling.FIT,
  cover: WireScaling.COVER,
  stretch: WireScaling.STRETCH,
  fitWidth: WireScaling.FIT_WIDTH,
  fitHeight: WireScaling.FIT_HEIGHT,
  none: WireScaling.NONE,
} as const

/** scaling encodes a way to fit a picture. */
function scaling(value: Scaling | undefined): WireScaling | undefined {
  return value === undefined ? undefined : scalings[value]
}

/** flows maps each flow to its wire value. */
const flows = {
  down: Flow.DOWN,
  right: Flow.RIGHT,
  downWrap: Flow.DOWN_WRAP,
  rightWrap: Flow.RIGHT_WRAP,
} as const

/** overflows maps each overflow to its wire value. */
const overflows = {
  squish: WireOverflow.SQUISH,
  clip: WireOverflow.CLIP,
  scroll: WireOverflow.SCROLL,
  show: WireOverflow.SHOW,
} as const

/** overflow encodes an overflow. */
function overflow(value: Overflow | undefined): WireOverflow | undefined {
  return value === undefined ? undefined : overflows[value]
}

/** shapes maps each gradient shape to its wire value. */
const shapes = {
  down: GradientShape.DOWN,
  right: GradientShape.RIGHT,
  up: GradientShape.UP,
  left: GradientShape.LEFT,
  radial: GradientShape.RADIAL,
} as const

/** weights maps each text weight to its wire value. */
const weights = {
  thin: Weight.THIN,
  light: Weight.LIGHT,
  normal: Weight.NORMAL,
  medium: Weight.MEDIUM,
  semiBold: Weight.SEMI_BOLD,
  bold: Weight.BOLD,
  black: Weight.BLACK,
} as const

/** textOverflows maps each text overflow to its wire value. */
const textOverflows = {
  ellipsis: TextOverflow.ELLIPSIS,
  shrink: TextOverflow.SHRINK,
  clip: TextOverflow.CLIP,
} as const

/** decorations maps each text decoration to its wire value. */
const decorations = {
  underline: Decoration.UNDERLINE,
  strike: Decoration.STRIKE,
} as const

/** easings maps each easing to its wire value. */
const easings = {
  out: Easing.EASE_OUT,
  in: Easing.EASE_IN,
  inOut: Easing.EASE_IN_OUT,
  linear: Easing.LINEAR,
} as const

/** wireStyle encodes style. */
function wireStyle(style: Style | undefined): WireStyle | undefined {
  if (!style) {
    return undefined
  }
  return {
    width: length(style.width),
    height: length(style.height),
    flow: style.flow === undefined ? undefined : flows[style.flow],
    horizontalAlign: align(style.horizontalAlign),
    verticalAlign: align(style.verticalAlign),
    margin: edges(style.margin),
    padding: edges(style.padding),
    background: color(style.background),
    color: color(style.color),
    fontSize: style.fontSize,
    bold: style.bold,
    textAlign: align(style.textAlign),
    borderRadius: style.borderRadius,
    opacity: style.opacity,
    face: style.face === undefined ? undefined : faces[style.face],
    uppercase: style.uppercase,
    letterSpacing: style.letterSpacing,
    shadow: style.shadow,
    borderWidth: style.borderWidth,
    borderColor: color(style.borderColor),
    x: length(style.x),
    y: length(style.y),
    minWidth: length(style.minWidth),
    maxWidth: length(style.maxWidth),
    minHeight: length(style.minHeight),
    maxHeight: length(style.maxHeight),
    zIndex: style.zIndex,
    overflowX: overflow(style.overflowX ?? style.overflow),
    overflowY: overflow(style.overflowY ?? style.overflow),
    gradient: gradient(style.gradient),
    boxShadow: shadow(style.boxShadow),
    textShadow: shadow(style.textShadow),
    tint: color(style.tint),
    saturation: style.saturation,
    brightness: style.brightness,
    contrast: style.contrast,
    blur: style.blur,
    italic: style.italic,
    weight: style.weight === undefined ? undefined : weights[style.weight],
    lineHeight: style.lineHeight,
    noWrap: style.noWrap,
    textOverflow:
      style.textOverflow === undefined
        ? undefined
        : textOverflows[style.textOverflow],
    decoration:
      style.decoration === undefined
        ? undefined
        : decorations[style.decoration],
    transform: transform(style),
    transition: transition(style.transition),
  }
}

/** gradient encodes a gradient, spacing colors without places evenly. */
function gradient(value: Gradient | undefined): WireGradient | undefined {
  if (!value) {
    return undefined
  }
  const last = Math.max(1, value.colors.length - 1)
  return {
    shape: value.shape === undefined ? undefined : shapes[value.shape],
    stops: value.colors.map((each, index) => ({
      at: value.at?.[index] ?? index / last,
      color: color(each),
    })),
  }
}

/** shadow encodes a shadow. */
function shadow(value: Shadow | undefined): WireShadow | undefined {
  if (!value) {
    return undefined
  }
  return {
    x: value.x,
    y: value.y,
    blur: value.blur,
    spread: value.spread,
    color: color(value.color),
    inset: value.inset,
  }
}

/** transform encodes a style's move, scale, turn and origin, or none. */
function transform(style: Style): WireTransform | undefined {
  const { translate, scale, rotate, origin } = style
  if (!translate && scale === undefined && rotate === undefined && !origin) {
    return undefined
  }
  const scales = typeof scale === 'number' ? [scale, scale] : scale
  return {
    translateX: translate?.[0],
    translateY: translate?.[1],
    scaleX: scales?.[0],
    scaleY: scales?.[1],
    rotate,
    originX: origin?.[0],
    originY: origin?.[1],
  }
}

/** transition encodes a transition: seconds, or a Transition. */
function transition(
  value: number | Transition | undefined,
): WireTransition | undefined {
  if (value === undefined) {
    return undefined
  }
  if (typeof value === 'number') {
    return { seconds: value }
  }
  return {
    seconds: value.seconds,
    delay: value.delay,
    easing: value.easing === undefined ? undefined : easings[value.easing],
  }
}

/** length encodes a length. */
function length(value: Length | undefined): WireLength | undefined {
  // Convert each length form to its wire case.
  if (value === undefined) {
    return undefined
  }
  if (typeof value === 'number') {
    return { value: { case: 'pixels', value } }
  }
  if (value === 'fit') {
    return { value: { case: 'fitChildren', value: true } }
  }
  if (value === 'fill') {
    return { value: { case: 'fill', value: 1 } }
  }
  return { value: { case: 'percent', value: Number.parseFloat(value) } }
}

/** edges encodes edges. */
function edges(value: Edges | undefined): WireEdges | undefined {
  if (value === undefined) {
    return undefined
  }
  if (typeof value === 'number') {
    return { top: value, right: value, bottom: value, left: value }
  }
  if (value.length === 2) {
    return { top: value[0], right: value[1], bottom: value[0], left: value[1] }
  }
  return { top: value[0], right: value[1], bottom: value[2], left: value[3] }
}

/** color encodes '#rrggbb' or '#rrggbbaa' as 0xRRGGBBAA. */
function color(value: string | undefined): number | undefined {
  const hex = value?.replace(/^#/, '')
  if (!hex || !/^[0-9a-f]{6}([0-9a-f]{2})?$/i.test(hex)) {
    return undefined
  }
  return Number.parseInt(hex.length === 6 ? `${hex}ff` : hex, 16) >>> 0
}
