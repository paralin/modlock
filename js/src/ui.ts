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

import {
  Align,
  type Change,
  Face,
  Flow,
  HudPart as WireHudPart,
  Kind,
  type Edges as WireEdges,
  type Length as WireLength,
  type Node,
  type Style as WireStyle,
  UiRequest,
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

/** Style lays out and paints an element. Colors are '#rrggbb' or '#rrggbbaa'. */
export interface Style {
  width?: Length
  height?: Length
  /** flow places the children in a column or a row; unset stacks them. */
  flow?: 'down' | 'right'
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
  /**
   * borderWidth and borderColor draw a line around the element's edge; it
   * shows only when both are set.
   */
  borderWidth?: number
  borderColor?: string
}

/**
 * HudPart is a part of the game's own HUD: 'levelUp' the notice for each boon,
 * ability point and power increase, 'ownHealth' the player's health bar with
 * its shields and regeneration, 'topBar' the bar of team scores, portraits
 * and clock across the top. The abilities, items, status icons, crosshair
 * and chat stay.
 */
export type HudPart = 'levelUp' | 'ownHealth' | 'topBar'

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
  readonly style?: Style
  readonly children?: Child
  readonly src?: string
  readonly data?: unknown
  readonly onPress?: (player: Player) => void
}

/** PanelProps lay out a panel, which groups its children. */
export interface PanelProps {
  style?: Style
  children?: Child
}

/** LabelProps show text: the label's text children, joined. */
export interface LabelProps {
  style?: Style
  children?: Child
}

/** ImageProps show an image from the game's content, such as 'file://{images}/hud/icon.png'. */
export interface ImageProps {
  style?: Style
  src: string
}

/**
 * ButtonProps make a button, which holds its children and calls onPress
 * with the player who pressed it. Players press buttons while they have a
 * cursor.
 */
export interface ButtonProps {
  style?: Style
  children?: Child
  onPress?: (player: Player) => void
}

/**
 * LayoutProps load a layout from the mod's content addon: src is its path
 * there, such as 'shop.xml', and data is the JSON value its script reads with
 * Modlock.data.
 */
export interface LayoutProps {
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
    button: ButtonProps & IntrinsicAttributes
    layout: LayoutProps & IntrinsicAttributes
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
 * Notices stack, newest last, and fade as they expire.
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

/** fadeSeconds is how long a notice takes to fade before it expires. */
const fadeSeconds = 0.5

/** view returns what the mod shows player, or an empty view. */
function view(player: Player): View {
  return (
    views.get(player.slot) ?? {
      player,
      root: undefined,
      hud: [],
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
  const next = render(previous, root, hud, current.routes)
  const change = diff(previous, next)

  // Send the change; a failed send means the player may have left, so send
  // the screen whole.
  if (change && !send(player.slot, change)) {
    const whole = render(undefined, root, hud, current.routes)
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
  const style: Style = {
    width: 'fit',
    height: 'fit',
    flow: 'down',
    horizontalAlign: 'left',
    verticalAlign: 'bottom',
    margin: [0, 0, 260, 24],
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
          // Step the fade by tenths so it sends a handful of changes.
          opacity:
            Math.ceil(Math.min(1, (notice.until - now) / fadeSeconds) * 10) /
            10,
        },
        children: notice.text,
      },
      notice.id,
    ),
  )
  return jsx('panel', { style, children: labels })
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
  routes: ReadonlyMap<string, string>,
): Screen {
  // The root is the full-screen panel the elements hang from, and it lists
  // the parts of the game's HUD to hide.
  const layout = new Layout(previous)
  layout.add({
    id: '',
    kind: Kind.PANEL,
    children: place(layout, root, '/'),
    hideHud: [...hud],
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
      next: previous?.next ?? 0,
    }
  }

  /** add records a node. */
  add(node: Node): void {
    this.screen.nodes.set(node.id!, { node, encoded: JSON.stringify(node) })
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
  }
  switch (child.type) {
    case 'label':
      node.kind = Kind.LABEL
      node.text = text(props.children)
      break
    case 'image':
      node.kind = Kind.IMAGE
      node.image = props.src ?? ''
      break
    case 'button':
      node.kind = Kind.BUTTON
      if (props.onPress) {
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
} as const

/** faces maps each typeface to its wire value. */
const faces = {
  body: Face.BODY,
  block: Face.BLOCK,
  serif: Face.SERIF,
} as const

/** wireStyle encodes style. */
function wireStyle(style: Style | undefined): WireStyle | undefined {
  if (!style) {
    return undefined
  }
  return {
    width: length(style.width),
    height: length(style.height),
    flow:
      style.flow === 'down'
        ? Flow.DOWN
        : style.flow === 'right'
          ? Flow.RIGHT
          : undefined,
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
