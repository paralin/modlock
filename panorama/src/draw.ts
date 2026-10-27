// Drawing turns each mod's tree into stock panels. It runs no mod code: every
// property it writes comes from a typed field of the tree, and a node becomes
// one of five stock panel types. A node keeps its panel across style changes,
// so a transition animates them: a property the new style drops returns to
// its resting value, and only a property with none makes a new panel. A
// node's hover style is drawn over its style while the pointer is over it. A layout node is the one exception: a host
// that has reviewed a mod's content addon passes Layouts, and the drawing
// loads the mod's layout into the node's panel; without Layouts the panel
// stays empty. A route node draws nothing and hands its value to each of the
// mod's layouts. It reaches the game only through the Panorama it is given,
// so a host's own interface script may draw the screens it receives another
// way.

import {
  Align,
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
  type Shadow,
  type Style,
  TextOverflow,
  type Transform,
  type Tree,
  Weight,
} from '../../src/wasm/ui/ui.pb.js'
import {
  isRouteName,
  layoutDataAttribute,
  layoutDataChanged,
  routeAttribute,
} from './data.js'

export {
  isRouteName,
  layoutDataAttribute,
  layoutDataChanged,
  routeAttribute,
} from './data.js'

/** Panel is the part of a game panel the drawing uses. */
export interface Panel {
  readonly paneltype: string
  hittest: boolean
  enabled: boolean
  text: string
  readonly style: Record<string, string | null>
  IsValid(): boolean
  GetChild(index: number): Panel | null
  MoveChildBefore(child: Panel, before: Panel): void
  MoveChildAfter(child: Panel, after: Panel): void
  SetImage(path: string): void
  SetPanelEvent(event: string, handler: () => void): void
  SetAttributeString(name: string, value: string): void
  BLoadLayout(
    path: string,
    overrideExisting: boolean,
    partial: boolean,
  ): boolean
  DeleteAsync(seconds: number): void
}

/**
 * Panorama is the part of the game's script API the drawing uses. CreatePanel
 * takes the properties a panel is made with only when there are some, since
 * the game refuses an undefined argument. PlaySound plays one of the game's
 * sound events; a host without sound leaves it out.
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
  PlaySound?(sound: string): void
}

/** Press reports a press on a mod's button. */
export type Press = (mod: string, node: string) => void

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
   * unloaded runs after the drawing deletes one or more layouts' panels. A
   * layout's key binds replaced the host's for the whole game process, so the
   * host binds its keys again.
   */
  unloaded(): void
}

/**
 * isLayoutPath reports whether path names a layout inside a mod's addon: one
 * or more segments of letters, digits, '_', '-' and '.', none of them "..",
 * ending in ".xml". The screen comes from the game server, so the drawing
 * checks the path before a host joins it to the addon's layout directory.
 */
export function isLayoutPath(path: string): boolean {
  return (
    path.endsWith('.xml') &&
    path.split('/').every((part) => /^[\w.-]+$/.test(part) && part !== '..')
  )
}

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
  [Kind.LAYOUT]: 'Panel',
  [Kind.ROUTE]: 'Panel',
  [Kind.HERO_IMAGE]: 'CitadelHeroImage',
}

/**
 * Drawn is one node's panel, its parent, the properties it was made with as
 * JSON, and its style: the node's own, its hover's, and what the panel holds
 * now, which is the hover's over the node's while the pointer is over it. It
 * keeps the node's pointer feedback for the panel's events, and whether its
 * tooltip shows. A layout node's panel also records the layout's URL, whether
 * it loaded, and the data set on it.
 */
interface Drawn {
  panel: Panel
  parent: Panel
  made: string
  style: Record<string, string>
  hover: Record<string, string>
  written: Record<string, string>
  over: boolean
  tooltip: string
  tipped: boolean
  sound: string
  hoverSound: string
  disabled: boolean
  layout: string | null
  loaded: boolean
  data: string
}

/** Drawing keeps the panels of every mod's tree under one parent. */
export class Drawing {
  /** mods holds each mod's drawn nodes by id; the root's id is empty. */
  private readonly mods = new Map<string, Map<string, Drawn>>()

  /** routes holds each mod's routes, the value of each by name. */
  private readonly routes = new Map<string, Map<string, string>>()

  constructor(
    private readonly panorama: Panorama,
    private readonly parent: Panel,
    private readonly press: Press,
    private readonly layouts?: Layouts,
  ) {}

  /** unloaded records that this draw deleted a layout's panel. */
  private unloaded = false

  /** changed records that this draw changed a loaded layout's data or routes. */
  private changed = false

  /** draw makes the panels match screen, keeping each panel whose node stays. */
  draw(screen: Screen | null): void {
    // Delete the trees of mods the screen no longer holds.
    const trees = new Map(
      (screen?.trees ?? []).map((tree) => [tree.mod ?? '', tree]),
    )
    for (const [mod, drawn] of this.mods) {
      if (!trees.has(mod)) {
        drawn.get('')?.panel.DeleteAsync(0)
        this.unloaded ||= [...drawn.values()].some(
          (each) => each.layout !== null,
        )
        this.mods.delete(mod)
        this.routes.delete(mod)
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

    // Draw the mod's tree from its root, reusing the panels drawn last time.
    const previous = this.mods.get(mod) ?? new Map<string, Drawn>()
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

    // A node no parent reaches is not drawn; its panel goes with its old parent.
    for (const [id, old] of previous) {
      if (!drawn.has(id) && old.panel.IsValid()) {
        this.delete(old)
      }
    }
    this.mods.set(mod, drawn)
  }

  /** delete deletes a drawn node's panel and its tooltip, and notes a layout's. */
  private delete(drawn: Drawn): void {
    this.untip(drawn)
    drawn.panel.DeleteAsync(0)
    this.unloaded ||= drawn.layout !== null
  }

  /**
   * paint writes the style the node's panel should hold now, the hover's over
   * the node's while the pointer is over it, returning each property it no
   * longer sets to its resting value.
   */
  private paint(drawn: Drawn): void {
    const target = drawn.over ? { ...drawn.style, ...drawn.hover } : drawn.style
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
    const drawn = this.mods.get(mod)?.get(id)
    if (!drawn || drawn.over === over || !drawn.panel.IsValid()) {
      return
    }

    // Tick, show or hide the tooltip, then repaint in the new state.
    drawn.over = over
    if (over && drawn.hoverSound) {
      this.panorama.PlaySound?.(drawn.hoverSound)
    }
    if (over && drawn.tooltip) {
      this.panorama.DispatchEvent(
        'UIShowTextTooltip',
        drawn.panel,
        drawn.tooltip,
      )
      drawn.tipped = true
    } else if (!over) {
      this.untip(drawn)
    }
    this.paint(drawn)
  }

  /** untip hides the tooltip a node's panel shows. */
  private untip(drawn: Drawn): void {
    if (drawn.tipped) {
      drawn.tipped = false
      this.panorama.DispatchEvent('UIHideTextTooltip', drawn.panel)
    }
  }

  /** activate presses a button that is not disabled, playing its sound. */
  private activate(mod: string, id: string): void {
    const drawn = this.mods.get(mod)?.get(id)
    if (drawn?.disabled) {
      return
    }
    if (drawn?.sound) {
      this.panorama.PlaySound?.(drawn.sound)
    }
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
  ): Panel {
    // Resolve the node's id, kind, and the URL and data of its layout.
    const id = node.id ?? ''
    const kind = id === '' ? Kind.PANEL : (node.kind ?? Kind.PANEL)
    const path = node.layout ?? ''
    const layout =
      kind === Kind.LAYOUT && isLayoutPath(path)
        ? (this.layouts?.url(mod, path) ?? null)
        : null
    const data = node.data ?? ''

    // Resolve its panel type, the properties it is made with, and its style
    // and hover style with their transition.
    const type = panelTypes[kind] ?? 'Panel'
    const properties = madeWith(kind, node)
    const made = JSON.stringify(properties)
    const style = {
      ...styleOf(node.style, id === ''),
      ...(region ? placed(region) : {}),
    }
    const hover = hoverOf(node.hover, style)
    Object.assign(style, transitionOf(node.style, style, hover))

    // Find the node's last panel and the style it now wants, with the hover
    // style over it while the pointer stays on it.
    const old = previous.get(id)
    const over = old?.over ?? false
    const target = over ? { ...style, ...hover } : style

    // Reuse the panel when its type, parent, properties and layout stay and
    // each property it holds that the new style drops has a resting value to
    // return to; or make one.
    let panel = old?.panel
    let written = old?.written ?? {}
    let loaded = old?.loaded ?? false
    let given = old?.data ?? ''
    const kept =
      old &&
      old.parent === parent &&
      old.made === made &&
      old.layout === layout &&
      Object.keys(written).every((name) => name in target || name in rest)
    if (!panel || !kept || !panel.IsValid() || panel.paneltype !== type) {
      if (old?.panel.IsValid()) {
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
        panel.SetPanelEvent('onactivate', () => this.activate(mod, id))
      }
      if (!layout) {
        panel.SetPanelEvent('onmouseover', () => this.point(mod, id, true))
        panel.SetPanelEvent('onmouseout', () => this.point(mod, id, false))
      }
    }

    // Load the node's layout with its data, or hand a loaded layout its new
    // data. A layout whose addon has not mounted yet loads on a later draw.
    if (layout && !loaded) {
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
    const each: Drawn = {
      panel,
      parent,
      made,
      style,
      hover,
      written,
      over,
      tooltip: node.tooltip ?? '',
      tipped: old?.panel === panel && old.tipped,
      sound: soundOf(node.sound),
      hoverSound: soundOf(node.hoverSound),
      disabled: kind === Kind.BUTTON && !!node.disabled,
      layout,
      loaded,
      data,
    }
    drawn.set(id, each)

    // Write the node's content and the style properties that changed. A
    // layout's own root decides whether it takes the pointer; a node takes it
    // to be pressed, to answer hover, or to scroll.
    if (!layout) {
      panel.hittest =
        kind === Kind.BUTTON ||
        Object.keys(hover).length !== 0 ||
        !!each.tooltip ||
        !!each.hoverSound ||
        /scroll/.test(style.overflow ?? '')
    }
    if (kind === Kind.BUTTON) {
      panel.enabled = !each.disabled
    }
    if (kind === Kind.LABEL) {
      panel.text = node.text ?? ''
    } else if (kind === Kind.IMAGE) {
      panel.SetImage(node.image ?? '')
    }
    this.paint(each)
    if (each.tipped && !each.tooltip) {
      this.untip(each)
    }

    // Draw each child once, in order; a child seen before would make a cycle.
    // A layout's panels come from its file, not from the tree.
    let last: Panel | undefined
    for (const child of kind === Kind.LAYOUT ? [] : (node.children ?? [])) {
      const childNode = nodes.get(child)
      if (!childNode || drawn.has(child) || child === '') {
        continue
      }
      const childPanel = this.drawNode(
        mod,
        childNode,
        panel,
        previous,
        drawn,
        nodes,
      )
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
        this.panorama.Msg(
          `[Modlock] the game refused the style ${name}: ${String(error)}`,
        )
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
): Record<string, string> {
  const css: Record<string, string | null> = {
    width: length(style?.width) ?? (root ? '100%' : null),
    height: length(style?.height) ?? (root ? '100%' : null),
    minWidth: length(style?.minWidth),
    maxWidth: length(style?.maxWidth),
    minHeight: length(style?.minHeight),
    maxHeight: length(style?.maxHeight),
    flowChildren: style?.flow ? (flows[style.flow] ?? null) : null,
    horizontalAlign: style?.horizontalAlign
      ? aligns[style.horizontalAlign][0]
      : null,
    verticalAlign: style?.verticalAlign ? aligns[style.verticalAlign][1] : null,
    x: length(style?.x),
    y: length(style?.y),
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
  }
  return Object.fromEntries(
    Object.entries(css).filter(
      (entry): entry is [string, string] => entry[1] !== null,
    ),
  )
}

/**
 * hoverOf returns the hover style's properties to draw over style: those that
 * differ from style and that style either sets or can return from, since the
 * hover lifts again without a new panel.
 */
function hoverOf(
  hover: Style | undefined,
  style: Record<string, string>,
): Record<string, string> {
  if (!hover) {
    return {}
  }
  return Object.fromEntries(
    Object.entries(styleOf(hover, false)).filter(
      ([name, value]) =>
        style[name] !== value && (name in style || name in rest),
    ),
  )
}

/**
 * transitionOf returns the properties that animate the changes of style and
 * hover over the style's transition: every property either writes that the
 * game animates.
 */
function transitionOf(
  source: Style | undefined,
  style: Record<string, string>,
  hover: Record<string, string>,
): Record<string, string> {
  // An element without a duration does not animate.
  const transition = source?.transition
  if (!transition?.seconds) {
    return {}
  }

  // Animate each property either style writes that the game can animate.
  const names = [...new Set([...Object.keys(style), ...Object.keys(hover)])]
    .filter((name) => animated.has(name))
    .map((name) => name.replace(/[A-Z]/g, (cap) => `-${cap.toLowerCase()}`))
  if (names.length === 0) {
    return {}
  }
  return {
    transitionProperty: names.join(', '),
    transitionDuration: `${clamp(transition.seconds, 0, 60)}s`,
    transitionDelay: `${clamp(transition.delay ?? 0, 0, 60)}s`,
    transitionTimingFunction: easings[transition.easing ?? Easing.EASE_OUT],
  }
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
  transform: 'none',
  transformOrigin: '50% 50%',
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
 * reads only then: an image's scaling, and a hero image's hero, picture and
 * scaling. Other panels take none.
 */
function madeWith(kind: Kind, node: Node): Record<string, string> | undefined {
  const scaling = node.scaling ? scalings[node.scaling] : undefined
  if (kind === Kind.HERO_IMAGE) {
    return {
      heroid: String(Math.trunc(finite(node.hero))),
      heroimagestyle: looks[node.heroLook ?? HeroLook.SMALL] ?? 'small',
      scaling: scaling ?? 'stretch-to-fit-preserve-aspect',
    }
  }
  return kind === Kind.IMAGE && scaling ? { scaling } : undefined
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
