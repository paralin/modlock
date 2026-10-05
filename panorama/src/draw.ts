// Drawing turns each mod's tree into stock panels. It runs no mod code: every
// property it writes comes from a typed field of the tree, and a node becomes
// one of four stock panel types. It reaches the game only through the
// Panorama it is given, so a host's own interface script may draw the screens
// it receives another way.

import { Align, type Edges, Flow, Kind, type Length, type Node, type Screen, type Style } from '../../proto/modlock/ui.pb.js'

/** Panel is the part of a game panel the drawing uses. */
export interface Panel {
  readonly paneltype: string
  hittest: boolean
  text: string
  readonly style: Record<string, string>
  IsValid(): boolean
  GetChild(index: number): Panel | null
  MoveChildBefore(child: Panel, before: Panel): void
  MoveChildAfter(child: Panel, after: Panel): void
  SetImage(path: string): void
  SetPanelEvent(event: string, handler: () => void): void
  DeleteAsync(seconds: number): void
}

/** Panorama is the part of the game's script API the drawing uses. */
export interface Panorama {
  CreatePanel(type: string, parent: Panel, id: string): Panel
  Msg(...values: unknown[]): void
}

/** Press reports a press on a mod's button. */
export type Press = (mod: string, node: string) => void

/**
 * pressCommand is the console command that presses node in mod's interface.
 * The game server hears it with the sending player, so any player's game can
 * press by running it.
 */
export function pressCommand(mod: string, node: string): string {
  return `modlockpress ${mod} ${node}`
}

/** panelTypes maps each kind of node to the stock panel that draws it. */
const panelTypes: Record<Kind, string> = {
  [Kind.PANEL]: 'Panel',
  [Kind.LABEL]: 'Label',
  [Kind.IMAGE]: 'Image',
  [Kind.BUTTON]: 'Button',
}

/** Drawn is one node's panel, its parent and the style written to it. */
interface Drawn {
  panel: Panel
  parent: Panel
  style: Record<string, string>
}

/** Drawing keeps the panels of every mod's tree under one parent. */
export class Drawing {
  /** mods holds each mod's drawn nodes by id; the root's id is empty. */
  private readonly mods = new Map<string, Map<string, Drawn>>()

  constructor(
    private readonly panorama: Panorama,
    private readonly parent: Panel,
    private readonly press: Press,
  ) {}

  /** draw makes the panels match screen, keeping each panel whose node stays. */
  draw(screen: Screen | null): void {
    const trees = new Map((screen?.trees ?? []).map((tree) => [tree.mod ?? '', tree.nodes ?? []]))
    for (const [mod, drawn] of this.mods) {
      if (!trees.has(mod)) {
        drawn.get('')?.panel.DeleteAsync(0)
        this.mods.delete(mod)
      }
    }
    for (const [mod, nodes] of trees) {
      this.drawTree(mod, new Map(nodes.map((node) => [node.id ?? '', node])))
    }
  }

  /** drawTree draws one mod's nodes from its root down. */
  private drawTree(mod: string, nodes: Map<string, Node>): void {
    const previous = this.mods.get(mod) ?? new Map<string, Drawn>()
    const drawn = new Map<string, Drawn>()
    const root = nodes.get('') ?? { id: '' }
    this.drawNode(mod, root, this.parent, previous, drawn, nodes)

    // A node no parent reaches is not drawn; its panel goes with its old parent.
    for (const [id, old] of previous) {
      if (!drawn.has(id) && old.panel.IsValid()) {
        old.panel.DeleteAsync(0)
      }
    }
    this.mods.set(mod, drawn)
  }

  /** drawNode draws node under parent, then its children in order. */
  private drawNode(
    mod: string,
    node: Node,
    parent: Panel,
    previous: Map<string, Drawn>,
    drawn: Map<string, Drawn>,
    nodes: Map<string, Node>,
  ): Panel {
    // Reuse the node's panel when its type and parent stay and its style
    // keeps every property, since Panorama cannot unset some of them; or make
    // one.
    const id = node.id ?? ''
    const kind = node.kind ?? Kind.PANEL
    const type = id === '' ? 'Panel' : panelTypes[kind] ?? 'Panel'
    const style = styleOf(node.style, id === '')
    const old = previous.get(id)
    let panel = old?.panel
    let written = old?.style ?? {}
    const kept = old && old.parent === parent && Object.keys(written).every((name) => name in style)
    if (!panel || !kept || !panel.IsValid() || panel.paneltype !== type) {
      if (panel?.IsValid()) {
        panel.DeleteAsync(0)
      }
      panel = this.panorama.CreatePanel(type, parent, id === '' ? `ModlockUi_${mod.replace(/\W/g, '_')}` : 'ModlockUiNode')
      written = {}
      if (kind === Kind.BUTTON && id !== '') {
        panel.SetPanelEvent('onactivate', () => this.press(mod, id))
      }
    }
    drawn.set(id, { panel, parent, style })

    // Write the node's content and the style properties that changed.
    panel.hittest = kind === Kind.BUTTON && id !== ''
    if (kind === Kind.LABEL) {
      panel.text = node.text ?? ''
    } else if (kind === Kind.IMAGE) {
      panel.SetImage(node.image ?? '')
    }
    for (const [name, value] of Object.entries(style)) {
      if (written[name] !== value) {
        this.write(panel, name, value)
      }
    }

    // Draw each child once, in order; a child seen before would make a cycle.
    let last: Panel | undefined
    for (const child of node.children ?? []) {
      const childNode = nodes.get(child)
      if (!childNode || drawn.has(child) || child === '') {
        continue
      }
      const childPanel = this.drawNode(mod, childNode, panel, previous, drawn, nodes)
      if (last) {
        panel.MoveChildAfter(childPanel, last)
      } else if (panel.GetChild(0) && panel.GetChild(0) !== childPanel) {
        panel.MoveChildBefore(childPanel, panel.GetChild(0)!)
      }
      last = childPanel
    }
    return panel
  }

  /** write sets one style property; a refused property leaves the rest drawn. */
  private write(panel: Panel, name: string, value: string): void {
    try {
      panel.style[name] = value
    } catch (error) {
      if (!refused.has(name)) {
        refused.add(name)
        this.panorama.Msg(`[Modlock] the game refused the style ${name}: ${String(error)}`)
      }
    }
  }
}

/** aligns maps each alignment to its horizontal and vertical style values. */
const aligns: Record<Align, [string, string]> = {
  [Align.START]: ['left', 'top'],
  [Align.CENTER]: ['center', 'center'],
  [Align.END]: ['right', 'bottom'],
}

/** styleOf returns the style properties to write; a root fills the screen by default. */
function styleOf(style: Style | undefined, root: boolean): Record<string, string> {
  const css: Record<string, string | null> = {
    width: length(style?.width) ?? (root ? '100%' : null),
    height: length(style?.height) ?? (root ? '100%' : null),
    flowChildren: style?.flow === Flow.DOWN ? 'down' : style?.flow === Flow.RIGHT ? 'right' : null,
    horizontalAlign: style?.horizontalAlign ? aligns[style.horizontalAlign][0] : null,
    verticalAlign: style?.verticalAlign ? aligns[style.verticalAlign][1] : null,
    margin: edges(style?.margin),
    padding: edges(style?.padding),
    backgroundColor: style?.background ? color(style.background) : null,
    color: style?.color ? color(style.color) : null,
    fontSize: style?.fontSize ? `${finite(style.fontSize)}px` : null,
    fontWeight: style?.bold ? 'bold' : null,
    textAlign: style?.textAlign ? aligns[style.textAlign][0] : null,
    borderRadius: style?.borderRadius ? `${finite(style.borderRadius)}px` : null,
    opacity: style?.opacity === undefined ? null : String(clamp(style.opacity, 0, 1)),
  }
  return Object.fromEntries(Object.entries(css).filter((entry): entry is [string, string] => entry[1] !== null))
}

/** refused holds the style properties the game refused, each logged once. */
const refused = new Set<string>()

/** length writes a length, or null for none. */
function length(value: Length | undefined): string | null {
  switch (value?.value?.case) {
    case 'pixels':
      return `${finite(value.value.value)}px`
    case 'percent':
      return `${clamp(value.value.value, 0, 100)}%`
    case 'fitChildren':
      return 'fit-children'
    case 'fill':
      return `fill-parent-flow(${clamp(value.value.value, 0, 100)})`
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
