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
// toast shows a player a short notice at the lower left, above the game's
// health and stats, which fades after a few seconds; a mod answers a command
// or refuses one with it.

import {
  Align,
  type Change,
  Flow,
  Kind,
  type Edges as WireEdges,
  type Length as WireLength,
  type Node,
  type Style as WireStyle,
} from '../../proto/modlock/ui.pb.js'
import { UiRequest } from '../../proto/modlock/wasm.pb.js'
import type { Player } from './host.gen.js'
import { call } from './host.js'

/**
 * Length is a size: pixels, a percentage of the parent, 'fit' to size to the
 * children, or 'fill' to share the space left in the parent's flow.
 */
export type Length = number | `${number}%` | 'fit' | 'fill'

/**
 * Edges are distances in pixels: one for every side, [vertical, horizontal],
 * or [top, right, bottom, left].
 */
export type Edges = number | readonly [number, number] | readonly [number, number, number, number]

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
}

/** Child is anything an element may hold. Text becomes a label. */
export type Child = UiElement | string | number | boolean | null | undefined | readonly Child[]

/** Component returns the elements that stand for its props. */
export type Component<P> = (props: P) => Child

/** UiElement is one element of an interface, as JSX builds it. */
export interface UiElement {
  readonly type: string | Component<never>
  readonly props: Readonly<Record<string, unknown>>
  readonly key?: string
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
  }
  interface IntrinsicAttributes {
    key?: string | number
  }
  interface ElementChildrenAttribute {
    children: unknown
  }
}

/** jsx builds one element; the TypeScript compiler calls it for JSX. */
export function jsx(type: string | Component<never>, props: Record<string, unknown>, key?: string | number): UiElement {
  return { type, props, key: key === undefined ? undefined : String(key) }
}

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

/** hide removes the interface and the notices the mod shows player. */
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
  views.set(player.slot, { ...current, notices: [...current.notices, notice].slice(-maxNotices) })
  return draw(player)
}

/** press runs the handler of the button the player pressed. */
export function press(player: Player, node: string): void {
  screens.get(player.slot)?.presses.get(node)?.(player)
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
  return views.get(player.slot) ?? { player, root: undefined, notices: [] }
}

/** draw sends what changed in player's interface and notices. */
function draw(player: Player): boolean {
  // Render the player's view and any notices against the last screen sent.
  const current = view(player)
  const root = [current.root, current.notices.length !== 0 && notices(current.notices)]
  const previous = screens.get(player.slot)
  const next = render(previous, root)
  const change = diff(previous, next)

  // Send the change; a failed send means the player may have left, so send
  // the screen whole.
  if (change && !send(player.slot, change)) {
    const whole = render(undefined, root)
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
          opacity: Math.ceil(Math.min(1, (notice.until - now) / fadeSeconds) * 10) / 10,
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
  /** presses maps each button's id to its handler. */
  readonly presses: Map<string, (player: Player) => void>
  /** next numbers the next new node, so an id is never reused. */
  next: number
}

/** screens holds each player's interface by slot. */
const screens = new Map<number, Screen>()

/** render lays root out as nodes, keeping the ids of elements that stayed in place. */
function render(previous: Screen | undefined, root: Child): Screen {
  // Start a screen that continues the previous screen's id counter.
  const screen: Screen = { nodes: new Map(), ids: new Map(), presses: new Map(), next: previous?.next ?? 0 }

  // id returns the node id for the element at place.
  const id = (place: string): string => {
    let found = previous?.ids.get(place)
    if (found === undefined) {
      found = (screen.next++).toString(36)
    }
    screen.ids.set(place, found)
    return found
  }

  // add records a node.
  const add = (node: Node) => {
    screen.nodes.set(node.id!, { node, encoded: JSON.stringify(node) })
  }

  // place lays out child at place and returns its nodes' ids.
  const place = (child: Child, at: string): string[] => {
    // Lay out empty values, lists, text, and function components.
    if (child === null || child === undefined || typeof child === 'boolean') {
      return []
    }
    if (Array.isArray(child)) {
      return (child as readonly Child[]).flatMap((item, index) => {
        const key = isElement(item) ? item.key : undefined
        return place(item, key === undefined ? `${at},${index}` : `${at},=${key}`)
      })
    }
    if (!isElement(child)) {
      const node: Node = { id: id(at), kind: Kind.LABEL, text: String(child) }
      add(node)
      return [node.id!]
    }
    if (typeof child.type === 'function') {
      return place((child.type as Component<unknown>)(child.props), `${at}>`)
    }

    // Draw an intrinsic element.
    const props = child.props
    const node: Node = { id: id(at), style: wireStyle(props.style as Style | undefined) }
    switch (child.type) {
      case 'label':
        node.kind = Kind.LABEL
        node.text = text(props.children as Child)
        break
      case 'image':
        node.kind = Kind.IMAGE
        node.image = String(props.src ?? '')
        break
      case 'button':
        node.kind = Kind.BUTTON
        if (typeof props.onPress === 'function') {
          screen.presses.set(node.id!, props.onPress as (player: Player) => void)
        }
        node.children = place(props.children as Child, `${at}/`)
        break
      default:
        node.kind = Kind.PANEL
        node.children = place(props.children as Child, `${at}/`)
    }
    add(node)
    return [node.id!]
  }

  // The root is the full-screen panel the elements hang from.
  add({ id: '', kind: Kind.PANEL, children: place(root, '/') })
  return screen
}

/** diff returns the change from previous to next, or undefined when nothing changed. */
function diff(previous: Screen | undefined, next: Screen): Change | undefined {
  const set = [...next.nodes.values()]
    .filter(({ node, encoded }) => previous?.nodes.get(node.id!)?.encoded !== encoded)
    .map(({ node }) => node)
  const removed = [...(previous?.nodes.keys() ?? [])].filter((id) => !next.nodes.has(id))
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

/** text joins a label's text children. */
function text(children: Child): string {
  if (children === null || children === undefined || typeof children === 'boolean') {
    return ''
  }
  if (Array.isArray(children)) {
    return (children as readonly Child[]).map(text).join('')
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

/** wireStyle encodes style. */
function wireStyle(style: Style | undefined): WireStyle | undefined {
  if (!style) {
    return undefined
  }
  return {
    width: length(style.width),
    height: length(style.height),
    flow: style.flow === 'down' ? Flow.DOWN : style.flow === 'right' ? Flow.RIGHT : undefined,
    horizontalAlign: style.horizontalAlign && aligns[style.horizontalAlign],
    verticalAlign: style.verticalAlign && aligns[style.verticalAlign],
    margin: edges(style.margin),
    padding: edges(style.padding),
    background: color(style.background),
    color: color(style.color),
    fontSize: style.fontSize,
    bold: style.bold,
    textAlign: style.textAlign && aligns[style.textAlign],
    borderRadius: style.borderRadius,
    opacity: style.opacity,
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
