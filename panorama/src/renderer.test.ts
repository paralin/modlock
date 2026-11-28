import type { JsonObject } from '@aptre/protobuf-es-lite'
import { beforeEach, expect, setSystemTime, test } from 'bun:test'

import {
  ActionPhase,
  GameAction,
  HudPart,
  Overlay,
  Screen,
} from '../../src/wasm/ui/ui.pb.js'
import { ClassAction, gameActionCommand, GameActions } from './actions.js'
import { Bridge } from './bridge.js'
import {
  actionCommand,
  AddonLayouts,
  CHAT_OPEN,
  Drawing,
  type InputValue,
  inputCommand,
  layoutDataChanged,
  layoutUnloadingAttribute,
  maxActionPayload,
  pressCommand,
} from './draw.js'
import {
  ALL_MARK,
  CONNECTED_TO_HIDEOUT,
  HIDDEN_MARK,
  HudParts,
  LEASE_SECONDS,
  StockGuard,
  WALK_SECONDS,
  BAR_GATE,
  SAME_CLICK_SECONDS,
  type GuardPanel,
} from './guard.js'
import { DISPLAY_COVER, DISPLAY_IMAGE, DRAWN_MARK } from './abilities.js'
import { mountInterfaceLayer } from './mount.js'
import {
  abilityBarMods,
  abilityRows,
  Cursor,
  cursorAsks,
  cursorFreed,
  gameActionMods,
  hiddenParts,
  overlayMods,
} from './hud.js'
import {
  overlayCommand,
  OverlayFollow,
  OverlayReader,
  stockOf,
} from './overlay.js'
import {
  maxPlacementLine,
  PlacementFollow,
  placementCommands,
} from './placement.js'
import { decodeUtf8 } from './packed.js'
import { Timelines } from './timeline.js'

/** FakePanel records what the renderer does to one panel. */
class FakePanel {
  children: FakePanel[] = []
  parent: FakePanel | null = null
  hittest = true
  enabled = true
  text = ''
  value = 0
  actuallayoutwidth = 200
  actualuiscale_x = 2
  actuallayoutheight = 0
  GetPositionWithinWindow() {
    return { x: 100, y: 0 }
  }
  checked = false
  /**
   * sheet is what the panel's style sheets draw under its classes: the
   * game's own switch, which never writes the inline value. Null is sheets
   * that name no visibility for the panel, where it keeps what it last drew.
   */
  get sheet(): boolean | null {
    return this.sheets
  }
  set sheet(shown: boolean | null) {
    this.sheets = shown
    this.paint()
  }
  private sheets: boolean | null = true
  /** drawn is what the panel draws, which Panorama keeps when nothing names it. */
  private drawn = true
  private paint() {
    const inline = this.style.visibility
    this.drawn =
      inline != null ? inline === 'visible' : (this.sheets ?? this.drawn)
  }
  image = ''
  url = ''
  layout = ''
  valid = true
  /** style repaints the panel on each visibility write, as Panorama does. */
  style = new Proxy<Record<string, string | null>>(
    {},
    {
      set: (target, name: string, value: string | null) => {
        target[name] = value
        if (name === 'visibility') this.paint()
        return true
      },
    },
  )
  events = new Map<string, () => void>()
  attributes = new Map<string, string>()
  classes: string[] = []
  classChanges: string[] = []
  dialog: Record<string, string> = {}
  options: FakePanel[] = []
  selected = ''
  deleteSeconds = -1
  focused = false

  BHasKeyFocus() {
    return this.focused
  }
  BHasDescendantKeyFocus(): boolean {
    return this.children.some(
      (child) => child.BHasKeyFocus() || child.BHasDescendantKeyFocus(),
    )
  }
  SetFocus() {
    this.focused = true
  }

  constructor(
    readonly paneltype: string,
    readonly id: string,
    readonly properties?: Record<string, string>,
  ) {}

  /**
   * visible is Panorama's: it reads the drawn visibility, the inline value
   * over the sheets, and writes the inline value, which then outlives every
   * class change.
   */
  get visible() {
    return this.drawn
  }
  set visible(shown: boolean) {
    this.style.visibility = shown ? 'visible' : 'collapse'
  }

  IsValid() {
    return this.valid
  }
  GetParent() {
    return this.parent
  }
  GetChildCount() {
    return this.children.length
  }
  GetChild(index: number) {
    return this.children[index] ?? null
  }
  MoveChildBefore(child: FakePanel, before: FakePanel) {
    this.children.splice(this.children.indexOf(child), 1)
    this.children.splice(this.children.indexOf(before), 0, child)
  }
  MoveChildAfter(child: FakePanel, after: FakePanel) {
    this.children.splice(this.children.indexOf(child), 1)
    this.children.splice(this.children.indexOf(after) + 1, 0, child)
  }
  SetImage(path: string) {
    this.image = path
  }
  SetURL(url: string) {
    this.url = url
  }
  SetPanelEvent(event: string, handler: () => void) {
    this.events.set(event, handler)
  }
  ClearPanelEvent(event: string) {
    this.events.delete(event)
  }
  SetAttributeString(name: string, value: string) {
    this.attributes.set(name, value)
  }

  GetAttributeString(name: string, fallback: string) {
    return this.attributes.get(name) ?? fallback
  }
  /** built builds the panels a loaded layout's file holds, if a test sets it. */
  static built: ((panel: FakePanel) => void) | null = null
  BLoadLayout(path: string) {
    FakePanel.built?.(this)
    this.layout = path
    return !path.includes('missing')
  }
  FindChild(id: string): FakePanel | null {
    return this.children.find((panel) => panel.id === id) ?? null
  }
  FindChildTraverse(id: string): FakePanel | null {
    return this.descendants().find((panel) => panel.id === id) ?? null
  }
  FindChildrenWithClassTraverse(name: string) {
    return this.descendants().filter((panel) => panel.classes.includes(name))
  }
  BHasClass(name: string) {
    return this.classes.includes(name)
  }
  AddClass(name: string) {
    this.classes.push(name)
    this.classChanges.push(`+${name}`)
  }
  RemoveClass(name: string) {
    this.classes.splice(this.classes.indexOf(name), 1)
    this.classChanges.push(`-${name}`)
  }
  descendants(): FakePanel[] {
    return this.children.flatMap((child) => [child, ...child.descendants()])
  }
  DeleteAsync(seconds: number) {
    this.valid = false
    this.deleteSeconds = seconds
    this.parent?.children.splice(this.parent.children.indexOf(this), 1)
  }
  SetDialogVariable(name: string, value: string) {
    this.dialog[name] = value
  }
  AddOption(option: FakePanel) {
    this.options.push(option)
  }
  SetSelected(id: string) {
    this.selected = id
  }
  GetSelected() {
    return this.options.find((option) => option.id === this.selected) ?? null
  }
}

/** handlers holds each panel's event handlers, as $.RegisterEventHandler keeps them. */
const handlers = new Map<
  FakePanel,
  Map<string, (...args: unknown[]) => unknown>
>()

/** scheduled holds the callbacks $.Schedule holds. */
const scheduled = new Map<number, () => void>()

/**
 * dispatched holds each event $.DispatchEvent sent, with its arguments, and
 * each sound played as a 'sound' event.
 */
const dispatched: string[][] = []

/** messages holds each line $.Msg logged. */
const messages: string[] = []

/** mouseX is where $.MousePosition puts the pointer across the window. */
let mouseX = 200

beforeEach(() => {
  // Forget the last test's calls and install a fake $.
  handlers.clear()
  scheduled.clear()
  dispatched.length = 0
  messages.length = 0
  mouseX = 200
  let handles = 0
  Object.assign(globalThis, {
    $: {
      Msg(...values: unknown[]) {
        messages.push(values.join(' '))
      },
      MousePosition() {
        return { x: mouseX, y: 0 }
      },
      CreatePanel(
        ...args: [string, FakePanel, string, Record<string, string>?]
      ) {
        // The game's $.CreatePanel throws on a properties argument that is
        // not an object.
        const [type, parent, id, properties] = args
        if (args.length > 3 && !properties)
          throw new Error('CreatePanel() properties argument is not an object')

        // Attach the panel to its parent.
        const panel = new FakePanel(type, id, properties)
        panel.parent = parent
        parent.children.push(panel)

        // A text entry holds its placeholder in a child label.
        if (type === 'TextEntry') {
          const placeholder = new FakePanel('Label', 'PlaceholderText')
          placeholder.parent = panel
          panel.children.push(placeholder)
        }
        return panel
      },
      RegisterEventHandler(
        event: string,
        panel: FakePanel,
        handler: (...args: unknown[]) => unknown,
      ) {
        handlers.set(
          panel,
          (
            handlers.get(panel) ??
            new Map<string, (...args: unknown[]) => unknown>()
          ).set(event, handler),
        )
      },
      Schedule(_: number, callback: () => void) {
        scheduled.set(++handles, callback)
        return handles
      },
      CancelScheduled(handle: number) {
        scheduled.delete(handle)
      },
      DispatchEvent(...args: string[]) {
        dispatched.push(args)
      },
      PlaySound(sound: string) {
        dispatched.push(['sound', sound])
      },
      Localize(token: string, panel: FakePanel) {
        return `${token} ${JSON.stringify(panel.dialog)}`
      },
    },
  })
})

/** runScheduled runs each callback $.Schedule holds now, once. */
function runScheduled() {
  const callbacks = [...scheduled.values()]
  scheduled.clear()
  for (const callback of callbacks) {
    callback()
  }
}

/** screen builds a screen with one mod's tree. */
function screen(...nodes: JsonObject[]): Screen {
  return Screen.fromJson({ trees: [{ mod: 'arena', nodes }] })
}

test('draws a tree, keeps panels that stay and reports presses', () => {
  // Draw onto a fake root panel and record each press.
  const root = new FakePanel('Panel', 'ModlockUi')
  const presses: string[] = []
  const drawing = new Drawing($, root, (mod, node) =>
    presses.push(`${mod}:${node}`),
  )

  // Draw a label and a button that holds a label.
  drawing.draw(
    screen(
      { id: '', children: ['a', 'b'] },
      {
        id: 'a',
        kind: 'KIND_LABEL',
        text: 'Round 1',
        style: { fontSize: 32, color: 0xff0000ff },
      },
      {
        id: 'b',
        kind: 'KIND_BUTTON',
        children: ['c'],
        style: { width: { percent: 150 }, margin: { top: 80 } },
      },
      { id: 'c', kind: 'KIND_LABEL', text: 'Ready' },
    ),
  )
  const tree = root.children[0]!
  const [label, button] = tree.children

  // The tree fills the root, and the label carries its text and style.
  expect(tree.style.width).toBe('100%')
  expect(label!.text).toBe('Round 1')
  expect(label!.style.fontSize).toBe('32px')
  expect(label!.style.color).toBe('rgba(255, 0, 0, 1.000)')
  expect(label!.hittest).toBe(false)

  // The button takes presses and caps its width at the tree's.
  expect(button!.paneltype).toBe('Button')
  expect(button!.hittest).toBe(true)
  expect(button!.style.width).toBe('100%')
  expect(button!.style.margin).toBe('80px 0px 0px 0px')
  expect(button!.children[0]!.text).toBe('Ready')

  // Pressing the button reports its mod and node.
  button!.events.get('onactivate')!()
  expect(presses).toEqual(['arena:b'])

  // Reordering keeps both panels; a dropped node's panel goes.
  drawing.draw(
    screen(
      { id: '', children: ['b', 'a'] },
      {
        id: 'a',
        kind: 'KIND_LABEL',
        text: 'Round 2',
        style: { fontSize: 40, color: 0xff0000ff },
      },
      {
        id: 'b',
        kind: 'KIND_BUTTON',
        style: { width: { percent: 50 }, margin: { top: 80 } },
      },
    ),
  )
  expect(tree.children).toEqual([button!, label!])
  expect(label!.text).toBe('Round 2')
  expect(label!.style.fontSize).toBe('40px')
  expect(button!.style.width).toBe('50%')
  expect(button!.children).toEqual([])

  // Panorama cannot unset some properties, so a node that drops one gets a
  // new panel.
  drawing.draw(
    screen(
      { id: '', children: ['a'] },
      { id: 'a', kind: 'KIND_LABEL', text: 'Round 3' },
    ),
  )
  expect(label!.valid).toBe(false)
  expect(tree.children.length).toBe(1)
  expect(tree.children[0]!.style).toEqual({})

  // A second mod's tree drawn in a region fills that box beside the first.
  drawing.draw(
    Screen.fromJson({
      trees: [
        { mod: 'arena', nodes: [{ id: '' }] },
        {
          mod: 'duel',
          nodes: [{ id: '' }],
          region: { left: 0.25, top: 0.5, width: 0.5, height: 0.25 },
        },
      ],
    }),
  )
  const placed = root.children[1]!
  expect(tree.style.x).toBeUndefined()
  expect(placed.style).toMatchObject({
    x: '25%',
    y: '50%',
    width: '50%',
    height: '25%',
  })

  // A fill takes the parent's flow only along it; across it, or in a parent
  // with no flow, it takes the whole parent, and a new flow redraws it.
  const filling = (flow: string) =>
    screen(
      { id: '', style: { flow }, children: ['f'] },
      {
        id: 'f',
        kind: 'KIND_PANEL',
        style: { width: { fill: 1 }, height: { fill: 1 } },
      },
    )
  drawing.draw(filling('FLOW_RIGHT'))
  const filled = root.children[0]!.children[0]!
  expect(filled.style.width).toBe('fill-parent-flow(1)')
  expect(filled.style.height).toBe('100%')

  // A column flips them.
  drawing.draw(filling('FLOW_DOWN'))
  expect(root.children[0]!.children[0]!.style.width).toBe('100%')
  expect(root.children[0]!.children[0]!.style.height).toBe(
    'fill-parent-flow(1)',
  )
  drawing.draw(filling('FLOW_NONE'))
  expect(root.children[0]!.children[0]!.style.width).toBe('100%')
  expect(root.children[0]!.children[0]!.style.height).toBe('100%')

  // A cycle draws each node once, and an empty screen removes the tree.
  drawing.draw(
    screen({ id: '', children: ['a'] }, { id: 'a', children: ['a', ''] }),
  )
  expect(tree.children.length).toBe(1)
  expect(tree.children[0]!.children).toEqual([])
  drawing.draw(null)
  expect(root.children).toEqual([])
})

test("draws the game's faces, styles, motion, hover and hero pictures", () => {
  // Draw a title in the heading face and a pill with an edge line.
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  drawing.draw(
    screen(
      { id: '', children: ['title', 'pill', 'bare'] },
      {
        id: 'title',
        kind: 'KIND_LABEL',
        text: 'Round 4',
        style: {
          face: 'FACE_BLOCK',
          uppercase: true,
          letterSpacing: 1,
          shadow: true,
        },
      },
      {
        id: 'pill',
        style: { borderWidth: 2, borderColor: 0xffefd71a },
      },
      { id: 'bare', style: { borderWidth: 2, face: 'FACE_DEFAULT' } },
    ),
  )
  const [title, pill, bare] = root.children[0]!.children

  // The title takes VALVEPulp in capitals over a drop shadow.
  expect(title!.style.fontFamily).toBe('VALVEPulp, "Noto Sans", sans-serif')
  expect(title!.style.textTransform).toBe('uppercase')
  expect(title!.style.letterSpacing).toBe('1px')
  expect(title!.style.textShadow).toBe('0px 1px 3px 1 #000000d0')

  // A border needs both its width and its color; the default face sets none.
  expect(pill!.style.border).toBe('2px solid rgba(255, 239, 215, 0.102)')
  expect(bare!.style).toEqual({})

  // Draw a button that answers the pointer, a hero's card, a wrapping
  // gradient row that scrolls, and a line of set text.
  const presses: string[] = []
  const live = new Drawing($, root, (mod, node) =>
    presses.push(`${mod}:${node}`),
  )
  const go = (extra: JsonObject = {}): JsonObject => ({
    id: 'go',
    kind: 'KIND_BUTTON',
    style: {
      background: 0x151611ff,
      transform: { scaleX: 1 },
      transition: { seconds: 0.15 },
    },
    hover: { background: 0x2b2a24ff, transform: { scaleX: 1.05 } },
    sound: 'UI.MainMenu.Activate',
    hoverSound: 'UI.MainMenu.Hover',
    tooltip: 'Start the round',
    ...extra,
  })
  const rest: JsonObject[] = [
    {
      id: 'hero',
      kind: 'KIND_HERO_IMAGE',
      hero: 15,
      heroLook: 'HERO_LOOK_CARD',
      scaling: 'SCALING_COVER',
    },
    {
      id: 'row',
      style: {
        flow: 'FLOW_RIGHT_WRAP',
        overflowY: 'OVERFLOW_SCROLL',
        x: { pixels: 12 },
        gradient: {
          shape: 'GRADIENT_SHAPE_RIGHT',
          stops: [
            { at: 0, color: 0x000000ff },
            { at: 1, color: 0xffffffff },
          ],
        },
        boxShadow: { y: 2, blur: 8, color: 0x00000080 },
        tint: 0xff0000ff,
        blur: 4,
      },
    },
    {
      id: 'line',
      kind: 'KIND_LABEL',
      text: 'Was 300',
      style: {
        weight: 'WEIGHT_SEMI_BOLD',
        italic: true,
        noWrap: true,
        textOverflow: 'TEXT_OVERFLOW_ELLIPSIS',
        decoration: 'DECORATION_STRIKE',
      },
    },
  ]
  const tree = (...nodes: JsonObject[]) =>
    screen({ id: '', children: ['go', 'hero', 'row', 'line'] }, ...nodes)
  live.draw(tree(go(), ...rest))
  const shown = () => root.children[root.children.length - 1]!.children
  const [button, hero, row, line] = shown()

  // The button animates the fill and transform both of its styles write.
  expect(button!.hittest).toBe(true)
  expect(button!.style).toMatchObject({
    backgroundColor: 'rgba(21, 22, 17, 1.000)',
    transform:
      'translate3d( 0px, 0px, 0px ) scale3d( 1, 1, 1 ) rotateZ( 0deg )',
    transitionProperty: 'background-color, transform',
    transitionDuration: '0.15s',
    transitionTimingFunction: 'ease-out',
  })

  // The pointer lights the button, ticks and shows the tooltip.
  dispatched.length = 0
  button!.events.get('onmouseover')!()
  expect(button!.style.backgroundColor).toBe('rgba(43, 42, 36, 1.000)')
  expect(button!.style.transform).toContain('scale3d( 1.05, 1.05, 1 )')

  // Leaving returns its style and hides the tooltip; a press plays its sound.
  button!.events.get('onmouseout')!()
  expect(button!.style.backgroundColor).toBe('rgba(21, 22, 17, 1.000)')
  button!.events.get('onactivate')!()
  expect(dispatched).toEqual([
    ['sound', 'UI.MainMenu.Hover'],
    ['UIShowTextTooltip', button, 'Start the round'],
    ['UIHideTextTooltip', button],
    ['sound', 'UI.MainMenu.Activate'],
  ] as unknown as string[][])
  expect(presses).toEqual(['arena:go'])

  // An item shows the shop's own card for it in place of the tooltip.
  live.draw(tree(go({ item: 630839635 }), ...rest))
  dispatched.length = 0
  button!.events.get('onmouseover')!()
  button!.events.get('onmouseout')!()
  expect(dispatched).toEqual([
    ['sound', 'UI.MainMenu.Hover'],
    ['CitadelShowModHoverTooltip', button, 630839635, 0],
    ['CitadelHideModHoverTooltip', button],
  ] as unknown as string[][])

  // Disabling the button and dropping its fill keep its panel, so the change
  // animates: the fill rests at none and presses stop.
  live.draw(
    tree(
      go({ disabled: true, style: { transition: { seconds: 0.15 } } }),
      ...rest,
    ),
  )
  expect(shown()[0]).toBe(button)
  expect(button!.enabled).toBe(false)
  expect(button!.style.backgroundColor).toBe('none')
  button!.events.get('onactivate')!()
  expect(presses).toEqual(['arena:go'])

  // The hero image is made with its hero, picture and scaling.
  expect(hero!.paneltype).toBe('CitadelHeroImage')
  expect(hero!.properties).toEqual({
    heroid: '15',
    heroimagestyle: 'hero_card',
    scaling: 'stretch-to-cover-preserve-aspect',
  })

  // The row wraps, scrolls down, sits at its offset and paints its gradient,
  // shadow, tint and blur.
  expect(row!.hittest).toBe(true)
  expect(row!.style).toMatchObject({
    flowChildren: 'right-wrap',
    overflow: 'squish scroll',
    x: '12px',
    backgroundColor:
      'gradient( linear, 0% 0%, 100% 0%, color-stop( 0, rgba(0, 0, 0, 1.000) ), color-stop( 1, rgba(255, 255, 255, 1.000) ) )',
    boxShadow: 'rgba(0, 0, 0, 0.502) 0px 2px 8px 0px',
    washColor: 'rgba(255, 0, 0, 1.000)',
    blur: 'gaussian(4px)',
  })

  // The line sets its weight, slant, wrap, overflow and strike.
  expect(line!.style).toMatchObject({
    fontWeight: 'semi-bold',
    fontStyle: 'italic',
    whiteSpace: 'nowrap',
    textOverflow: 'ellipsis',
    textDecoration: 'line-through',
  })

  // Replacing a lifetime dismisses a child tooltip before dropping its root.
  live.draw(tree(go(), ...rest))
  const hovered = shown()[0]!
  hovered.events.get('onmouseover')!()
  const restarted = tree(go(), ...rest)
  restarted.trees![0]!.generation = 2n
  dispatched.length = 0
  live.draw(restarted)
  expect(dispatched).toEqual([
    ['UIHideTextTooltip', hovered],
  ] as unknown as string[][])

  // Removing the whole mod dismisses the replacement's tooltip too.
  const replacement = shown()[0]!
  replacement.events.get('onmouseover')!()
  dispatched.length = 0
  live.draw(null)
  expect(dispatched).toEqual([
    ['UIHideTextTooltip', replacement],
  ] as unknown as string[][])
})

test("opens the game's ability window over a node and closes it", () => {
  // A plain panel with an ability takes the pointer, and its window wins over
  // its tooltip and its item's card, with the numbers of the slot's hero.
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  const cell = (extra: JsonObject = {}): JsonObject => ({
    id: 'cell',
    tooltip: 'Hook',
    item: 630839635,
    ability: 1234,
    abilityPlayer: 0,
    ...extra,
  })
  const tree = (...nodes: JsonObject[]) =>
    screen(
      { id: '', children: nodes.map((node) => node.id as string) },
      ...nodes,
    )
  drawing.draw(tree({ id: 'cell', ability: 1234 }))
  const panel = root.children[0]!.children[0]!
  expect(panel.hittest).toBe(true)
  drawing.draw(tree(cell()))
  panel.events.get('onmouseover')!()
  panel.events.get('onmouseout')!()
  expect(dispatched).toEqual([
    ['CitadelShowAbilityDetailsTooltip', panel, 1234, 0],
    ['CitadelHideAbilityDetailsTooltip', panel],
  ] as unknown as string[][])

  // Another hero's numbers, or another ability, open the window anew under
  // the pointer.
  dispatched.length = 0
  panel.events.get('onmouseover')!()
  drawing.draw(tree(cell({ abilityPlayer: 3 })))
  expect(root.children[0]!.children[0]).toBe(panel)
  drawing.draw(tree(cell({ abilityPlayer: 3, ability: 5678 })))
  drawing.draw(tree(cell({ abilityPlayer: 3, ability: 5678 })))
  expect(dispatched).toEqual([
    ['CitadelShowAbilityDetailsTooltip', panel, 1234, 0],
    ['CitadelHideAbilityDetailsTooltip', panel],
    ['CitadelShowAbilityDetailsTooltip', panel, 1234, 3],
    ['CitadelHideAbilityDetailsTooltip', panel],
    ['CitadelShowAbilityDetailsTooltip', panel, 5678, 3],
  ] as unknown as string[][])

  // Removing the node closes its window.
  dispatched.length = 0
  drawing.draw(tree({ id: 'other' }))
  expect(dispatched).toEqual([
    ['CitadelHideAbilityDetailsTooltip', panel],
  ] as unknown as string[][])

  // Leaving the game closes every window still open.
  const before = [...root.children[0]!.children]
  drawing.draw(tree(cell()))
  const again = root.children[0]!.children.find(
    (each) => !before.includes(each),
  )!
  again.events.get('onmouseover')!()
  dispatched.length = 0
  drawing.release()
  drawing.release()
  expect(dispatched).toEqual([
    ['CitadelHideAbilityDetailsTooltip', again],
  ] as unknown as string[][])
})

test('plays each cue once, cuts it on time and stops it as the player leaves', () => {
  // Play through a fake game that numbers each sound it starts.
  const sounds: string[] = []
  let started = 0
  const drawing = new Drawing(
    {
      ...$,
      PlaySound(sound: string) {
        sounds.push(`play ${sound} ${++started}`)
        return started
      },
      StopSound(playing: number) {
        sounds.push(`stop ${playing}`)
      },
    },
    new FakePanel('Panel', 'ModlockUi'),
    () => {},
  )
  const cues = (generation: number, ...list: [number, string, number?][]) =>
    Screen.fromJson({
      trees: [
        {
          mod: 'arena',
          generation: String(generation),
          nodes: [
            {
              id: '',
              cues: list.map(([number, sound, seconds]) => ({
                number,
                sound,
                seconds: seconds ?? 0,
              })),
            },
          ],
        },
      ],
    })

  // The first screen's cues may be long over and play nothing; each later
  // cue plays once, in order, however many screens repeat it.
  drawing.draw(cues(1, [1, 'Old']))
  drawing.draw(cues(1, [1, 'Old'], [3, 'Third'], [2, 'Second', 0.5]))
  drawing.draw(cues(1, [1, 'Old'], [3, 'Third'], [2, 'Second', 0.5]))
  expect(sounds).toEqual(['play Second 1', 'play Third 2'])

  // A cut stops its sound once, on time, and not again on leaving.
  expect(scheduled.size).toBe(1)
  runScheduled()
  expect(sounds.slice(2)).toEqual(['stop 1'])

  // A sound stopped by leaving is not cut again.
  sounds.length = 0
  drawing.draw(cues(1, [3, 'Third'], [4, 'Short', 0.5]))
  drawing.release()
  runScheduled()
  expect(sounds).toEqual(['play Short 3', 'stop 2', 'stop 3'])

  // A new lifetime plays all its cues and stops the old one's sounds.
  sounds.length = 0
  drawing.draw(cues(1, [4, 'Short', 0.5], [5, 'Long']))
  drawing.draw(cues(2, [1, 'Fresh']))
  expect(sounds).toEqual(['play Long 4', 'stop 4', 'play Fresh 5'])

  // Leaving stops what plays; the screens that follow play nothing old.
  sounds.length = 0
  drawing.release()
  drawing.draw(cues(2, [1, 'Fresh']))
  expect(sounds).toEqual(['stop 5'])

  // A mod gone from the screen stops its sounds and, back, plays only the
  // cues after its first screen, not those it sent while gone.
  sounds.length = 0
  drawing.draw(cues(2, [1, 'Fresh'], [2, 'Next']))
  drawing.draw(null)
  drawing.draw(cues(2, [1, 'Fresh'], [2, 'Next'], [3, 'Missed']))
  drawing.draw(cues(2, [1, 'Fresh'], [2, 'Next'], [3, 'Missed'], [4, 'Last']))
  expect(sounds).toEqual(['play Next 6', 'stop 6', 'play Last 7'])
})

test('runs a timeline: restarts it, cancels it and joins it on reload', () => {
  // Draw a banner a timeline shows at once, dims at 1s and sounds at 2s;
  // each step's look replaces the last one's for its node.
  setSystemTime(new Date(1_000_000))
  const sounds: string[] = []
  const root = new FakePanel('Panel', 'ModlockUi')
  const game = {
    ...$,
    PlaySound(sound: string) {
      sounds.push(sound)
      return sounds.length
    },
    StopSound() {},
  }
  const drawing = new Drawing(game, root, () => {})
  const timed = (run: number | null, generation = 1) =>
    Screen.fromJson({
      trees: [
        {
          mod: 'arena',
          generation: String(generation),
          nodes: [
            {
              id: '',
              children: ['banner'],
              timelines:
                run === null
                  ? []
                  : [
                      {
                        id: 'intro',
                        run,
                        steps: [
                          { at: 0, measure: 'banner', classes: ['Shown'] },
                          {
                            at: 1,
                            measure: 'banner',
                            classes: ['Shown'],
                            style: { opacity: 0.5 },
                          },
                          { at: 2, sound: 'Gong' },
                        ],
                      },
                    ],
            },
            {
              id: 'banner',
              kind: 'KIND_LABEL',
              text: 'Fight',
              measure: 'banner',
            },
          ],
        },
      ],
    })
  const banner = () => root.children[0]!.children[0]!

  // The first screen joins the run at its end: its looks, and no sound.
  drawing.draw(timed(1))
  expect(banner().classes).toContain('Shown')
  expect(banner().style.opacity).toBe('0.5')
  expect(sounds).toEqual([])

  // A rise in run clears the last looks and plays the steps on time.
  drawing.draw(timed(2))
  expect(banner().classes).toContain('Shown')
  expect(banner().style.opacity).toBe('1')
  setSystemTime(new Date(1_001_000))
  runScheduled()
  expect(banner().style.opacity).toBe('0.5')
  expect(sounds).toEqual([])

  // A restart mid-run starts over, and a cancel, which drops the timeline, before the sound takes back
  // every look and plays nothing.
  drawing.draw(timed(3))
  expect(banner().style.opacity).toBe('1')
  drawing.draw(timed(null))
  setSystemTime(new Date(1_005_000))
  runScheduled()
  expect(banner().classes).not.toContain('Shown')
  expect(sounds).toEqual([])

  // A run that finishes plays its sound once and keeps its last looks.
  drawing.draw(timed(4))
  setSystemTime(new Date(1_008_000))
  runScheduled()
  runScheduled()
  expect(sounds).toEqual(['Gong'])
  expect(banner().classes).toContain('Shown')

  // A new lifetime plays its runs from the start.
  drawing.draw(timed(1, 2))
  expect(banner().style.opacity).toBe('1')
  setSystemTime(new Date(1_011_000))
  runScheduled()
  runScheduled()
  expect(sounds).toEqual(['Gong', 'Gong'])

  // A mod gone from the screen takes its looks with it.
  drawing.draw(null)
  drawing.draw(timed(1, 2))
  expect(sounds).toEqual(['Gong', 'Gong'])
})

test("holds a game-clock timeline while the host's screen is paused", () => {
  // A mod's lifetime starts while the game is paused, then runs a timeline on
  // the game's clock whose one step comes a second in.
  setSystemTime(new Date(1_000_000))
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  const held = (paused: boolean, timed: boolean) =>
    Screen.fromJson({
      paused,
      trees: [
        {
          mod: 'arena',
          generation: '1',
          nodes: [
            {
              id: '',
              children: ['banner'],
              timelines: timed
                ? [
                    {
                      id: 'intro',
                      run: 1,
                      clock: 'TIMELINE_CLOCK_GAME',
                      steps: [{ at: 1, measure: 'banner', classes: ['Shown'] }],
                    },
                  ]
                : [],
            },
            { id: 'banner', kind: 'KIND_LABEL', measure: 'banner' },
          ],
        },
      ],
    })
  const banner = () => root.children[0]!.children[0]!
  drawing.draw(held(true, false))
  drawing.draw(held(true, true))

  // Five seconds of pause show nothing; the resume runs the last second.
  setSystemTime(new Date(1_005_000))
  runScheduled()
  expect(banner().classes).not.toContain('Shown')
  drawing.draw(held(false, true))
  setSystemTime(new Date(1_006_500))
  runScheduled()
  expect(banner().classes).toContain('Shown')
})

test('holds a game-clock timeline through a pause and not a wall-clock one', () => {
  // Run two timelines with one step at 2s on a clock the test moves.
  let time = 0
  const waits: (() => void)[] = []
  const looked: string[] = []
  const timelines = new Timelines(
    {
      schedule: (_, callback) => waits.push(callback),
      now: () => time,
      look: (node) => looked.push(node),
      play: () => {},
    },
    false,
  )
  const step = (measure: string) => [{ at: 2, measure, classes: ['On'] }]
  timelines.draw([
    { id: 'game', run: 1, clock: 1, steps: step('a') },
    { id: 'wall', run: 1, clock: 0, steps: step('b') },
  ] as never)
  const wake = () => waits.splice(0).forEach((callback) => callback())

  // A second in, the game pauses for five; only the wall clock moves on.
  time = 1
  timelines.pause(true)
  expect(timelines.state('game')).toBe('held')
  time = 6
  wake()
  expect(looked).toEqual(['b'])
  expect(timelines.state('wall')).toBe('done')

  // Unpaused, the game clock needs its last second.
  timelines.pause(false)
  wake()
  expect(timelines.state('game')).toBe('running')
  time = 7
  wake()
  expect(looked).toEqual(['b', 'a'])
  expect(timelines.state('game')).toBe('done')
})

test("runs a label's clock in the game and restarts it when the mod changes it", () => {
  // Draw a countdown from 65 seconds and a count up in tenths.
  setSystemTime(new Date(1_000_000))
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  const clocks = (run: number) =>
    screen(
      { id: '', children: ['down', 'up', 'token'] },
      {
        id: 'down',
        kind: 'KIND_LABEL',
        clock: { seconds: 65, run },
      },
      {
        id: 'up',
        kind: 'KIND_LABEL',
        clock: { seconds: 59.95, up: true, format: 'CLOCK_FORMAT_TENTHS' },
      },
      {
        id: 'token',
        kind: 'KIND_LABEL',
        token: '#Round',
        clock: { seconds: 3, format: 'CLOCK_FORMAT_SECONDS', paused: true },
      },
    )
  drawing.draw(clocks(1))
  const [down, up, token] = root.children[0]!.children
  expect(down!.text).toBe('1:05')
  expect(up!.text).toBe('59.9')
  expect(token!.dialog).toEqual({ clock: '3' })

  // The game ticks it on its own; a countdown shows zero only at its end.
  setSystemTime(new Date(1_000_000 + 64_500))
  runScheduled()
  expect(down!.text).toBe('0:01')
  expect(up!.text).toBe('2:04.4')
  setSystemTime(new Date(1_000_000 + 70_000))
  runScheduled()
  expect(down!.text).toBe('0:00')

  // The same clock drawn again keeps counting; a new run starts over.
  drawing.draw(clocks(1))
  expect(down!.text).toBe('0:00')
  drawing.draw(clocks(2))
  expect(down!.text).toBe('1:05')
  setSystemTime()
})

test('draws inputs, avatars and game text, and sends edits', () => {
  // Draw an entry, a slider, a toggle, a dropdown, an avatar and a token.
  const root = new FakePanel('Panel', 'ModlockUi')
  const changes: string[] = []
  const drawing = new Drawing(
    $,
    root,
    (mod, node) => changes.push(pressCommand(mod, node)),
    undefined,
    (mod, node, value) => changes.push(inputCommand(mod, node, value)),
  )
  const tree = (name: string, disabled = false) =>
    screen(
      {
        id: '',
        children: ['name', 'volume', 'ready', 'team', 'face', 'hero', 'save'],
      },
      {
        id: 'name',
        kind: 'KIND_INPUT',
        disabled,
        textEntry: { text: name, placeholder: 'Name', maxLength: 12 },
      },
      {
        id: 'volume',
        kind: 'KIND_INPUT',
        slider: { value: 5, min: 0, max: 10, step: 1 },
      },
      { id: 'ready', kind: 'KIND_INPUT', text: 'Ready', toggle: {} },
      {
        id: 'team',
        kind: 'KIND_INPUT',
        dropdown: { options: ['Amber', 'Sapphire'], selected: 1 },
      },
      { id: 'face', kind: 'KIND_AVATAR', steamId: '76561198000000001' },
      {
        id: 'hero',
        kind: 'KIND_LABEL',
        token: '#Citadel_HeroName_Haze',
        variables: { count: '3' },
      },
      { id: 'save', kind: 'KIND_BUTTON', text: 'Save' },
    )
  drawing.draw(tree('Ana'))
  const [name, volume, ready, team, face, hero, save] =
    root.children[0]!.children

  // Each control is made with its bounds and shows the mod's value.
  expect(name!.paneltype).toBe('TextEntry')
  expect(name!.properties).toEqual({ maxchars: '12', placeholder: 'Name' })
  expect(name!.text).toBe('Ana')
  expect(volume!.properties).toMatchObject({
    min: '0',
    max: '1',
    increment: '0.1',
  })
  expect(volume!.value).toBe(0.5)

  // The toggle carries its label and the dropdown its options.
  expect(ready!.paneltype).toBe('ToggleButton')
  expect(ready!.properties).toEqual({ text: 'Ready' })
  expect(team!.options.map((option) => option.properties?.text)).toEqual([
    'Amber',
    'Sapphire',
  ])
  expect(team!.selected).toBe('option1')

  // The avatar names the account, and the label shows the game's text.
  expect(face!.paneltype).toBe('CitadelAvatarImage')
  expect(face!.properties).toEqual({ accountid: '39734273' })
  expect(hero!.text).toBe('#Citadel_HeroName_Haze {"count":"3"}')

  // The entry wears the game's text box, and its placeholder hides under
  // text and shows again once the player clears it.
  expect(name!.style).toMatchObject({
    height: '44px',
    border: '2px solid rgba(198, 198, 198, 0.200)',
    color: 'rgba(255, 239, 215, 1.000)',
    fontSize: '18px',
  })
  const placeholder = name!.FindChildTraverse('PlaceholderText')!
  expect(placeholder.visible).toBe(false)
  expect(placeholder.style).toMatchObject({
    color: 'rgba(255, 239, 215, 1.000)',
    opacity: '0.5',
  })
  name!.text = ''
  name!.events.get('ontextentrychange')!()
  expect(placeholder.visible).toBe(true)

  // Focus brightens the border until the entry lets the keyboard go, and
  // letting it go sends the cleared text at once.
  name!.events.get('onfocus')!()
  expect(name!.style.border).toBe('2px solid rgba(255, 239, 215, 0.700)')
  name!.events.get('onblur')!()
  expect(name!.style.border).toBe('2px solid rgba(198, 198, 198, 0.200)')

  // A run of typing sends once, after a tenth of a second.
  name!.text = 'Anaïs'
  name!.events.get('ontextentrychange')!()
  name!.events.get('ontextentrychange')!()

  // A toggle and a choice send at once.
  ready!.checked = true
  ready!.events.get('onactivate')!()
  team!.selected = 'option0'
  team!.events.get('oninputsubmit')!()
  runScheduled()
  expect(changes).toEqual([
    // Letting the keyboard go committed the cleared entry.
    'modlockinput arena name ',
    'modlockinput arena ready 1',
    'modlockinput arena team 0',
    `modlockinput arena name ${Buffer.from('Anaïs').toString('base64url')}`,
  ])

  // A press right after typing carries the typed text first, and an entry
  // that lets the keyboard go sends what it shows at once.
  changes.length = 0
  name!.text = 'Anaïs?'
  name!.events.get('ontextentrychange')!()
  save!.events.get('onactivate')!()
  name!.text = 'Anaïs'
  name!.events.get('ontextentrychange')!()
  name!.events.get('onblur')!()
  runScheduled()
  expect(changes).toEqual([
    `modlockinput arena name ${Buffer.from('Anaïs?').toString('base64url')}`,
    'modlockpress arena save',
    `modlockinput arena name ${Buffer.from('Anaïs').toString('base64url')}`,
  ])

  // The mod drawing back what the player sent leaves the letters typed since
  // in place.
  name!.text = 'Anaïs!'
  drawing.draw(tree('Anaïs'))
  expect(name!.text).toBe('Anaïs!')

  // The mod's own value replaces them, and a disabled entry sends nothing.
  drawing.draw(tree('Bo', true))
  expect(name!.text).toBe('Bo')
  expect(placeholder.visible).toBe(false)
  expect(name!.style.color).toBe('rgba(255, 239, 215, 0.500)')
  name!.events.get('ontextentrychange')!()
  runScheduled()
  expect(changes).toHaveLength(3)

  // Text past the host's limit is cut at a character.
  const long = inputCommand('arena', 'name', {
    case: 'text',
    value: 'é'.repeat(200),
  } satisfies InputValue)
  expect(long).toBe(
    `modlockinput arena name ${Buffer.from('é'.repeat(120)).toString('base64url')}`,
  )

  // A node that is no id or a mod that is no slug, which could start a
  // command of its own, has an empty command.
  const on = { case: 'on', value: true } satisfies InputValue
  for (const node of ['', 'a b', 'a;quit', 'a\nquit', 'x'.repeat(65)]) {
    expect(pressCommand('arena', node)).toBe('')
    expect(inputCommand('arena', node, on)).toBe('')
  }
  for (const mod of ['', 'a;quit', 'a\nquit', 'Arena', '..', 'a--b']) {
    expect(pressCommand(mod, 'ability:0')).toBe('')
    expect(inputCommand(mod, 'ready', on)).toBe('')
  }
  expect(pressCommand('street-brawl', 'route:scores.v2')).toBe(
    'modlockpress street-brawl route:scores.v2',
  )
})

test("a layout's press names its mod, node and action and carries a short payload", () => {
  // The payload travels as base64url of its UTF-8; an empty one is left off.
  expect(actionCommand('arena', 'card', 'vote', '{"vote":1}')).toBe(
    'modlockaction arena card vote eyJ2b3RlIjoxfQ',
  )
  expect(actionCommand('arena', 'card', 'close', '')).toBe(
    'modlockaction arena card close',
  )
  expect(actionCommand('arena', 'card', 'keep', 'é'.repeat(90))).toBe(
    `modlockaction arena card keep ${Buffer.from('é'.repeat(90)).toString('base64url')}`,
  )

  // A payload past the limit, or a name the host refuses, sends nothing.
  expect(actionCommand('arena', 'card', 'keep', 'é'.repeat(91))).toBe('')
  expect(
    actionCommand('arena', 'card', 'keep', 'x'.repeat(maxActionPayload + 1)),
  ).toBe('')
  for (const action of ['', 'a b', 'a;quit', 'a\nquit', 'x'.repeat(65)]) {
    expect(actionCommand('arena', 'card', action, '')).toBe('')
  }
  expect(actionCommand('Arena', 'card', 'vote', '')).toBe('')
  expect(actionCommand('arena', 'a;quit', 'vote', '')).toBe('')
})

test('ranges follow a dragged handle and report it by name', () => {
  // Set up a renderer that reports the values it sends, under a stock
  // overlay the test opens.
  const root = new FakePanel('Panel', 'ModlockUi')
  const changes: InputValue[] = []
  let stock: 'escape' | 'other' | null = null
  const drawing = new Drawing(
    $,
    root,
    () => {},
    undefined,
    (_mod, _node, value) => changes.push(value),
    {
      allowed: () => true,
      stock: () => stock,
      capture: () => {},
      resume: () => {},
    },
  )
  const delay = (values: number[], disabled = false) =>
    screen(
      { id: '', children: ['delay'] },
      {
        id: 'delay',
        kind: 'KIND_INPUT',
        disabled,
        range: {
          handles: ['min', 'favored', 'max'].map((name, index) => ({
            name,
            value: values[index]!,
          })),
          max: 1,
          step: 0.25,
        },
      },
    )
  // The track starts at x 100 and spans 400 window pixels.
  // The track starts at window pixel 100 and is 200 window pixels wide.
  const point = (fraction: number) => {
    mouseX = 100 + fraction * 200
  }
  // Raising a knob reorders the panel's children, so read the knobs in
  // handle order as the control built them.
  let built: FakePanel[] = []
  const knobs = (panel: FakePanel) => {
    built = panel.children.filter((child) => child.id === 'RangeKnob')
    return built
  }
  const shown = () => built.map((knob) => knob.style.x)
  const grab = (knob: FakePanel) =>
    handlers.get(knob)!.get('DragStart')!('knob', {
      displayPanel: null,
      offsetX: 0,
      offsetY: 0,
    })
  const drop = (knob: FakePanel) => handlers.get(knob)!.get('DragEnd')!()

  // Each knob sits at its value's share of the track.
  drawing.draw(delay([0, 0.5, 1]))
  let panel = root.children[0]!.children[0]!
  const [low, favored, high] = knobs(panel)
  expect(shown()).toEqual(['0.000%', '50.000%', '100.000%'])
  expect(favored!.properties).toEqual({ draggable: 'true' })

  // A grab reports the handle and moves it to the pointer at once, snapped.
  point(0.3)
  expect(grab(favored!)).toBe(true)
  expect(changes).toEqual([
    {
      case: 'range',
      value: { phase: ActionPhase.PRESSED, handle: 'favored', values: [] },
    },
  ])
  expect(shown()[1]).toBe('25.000%')

  // Past the track's end, the handle stops at its neighbor, and a redraw
  // leaves the held handle where the player has it.
  point(1.4)
  runScheduled()
  expect(shown()[1]).toBe('100.000%')
  drawing.draw(delay([0, 0.75, 1]))
  expect(root.children[0]!.children[0]).toBe(panel)
  runScheduled()
  expect(shown()[1]).toBe('100.000%')

  // Letting go reports every value, and ends the drag's panel.
  expect(drop(favored!)).toBe(true)
  expect(changes[changes.length - 1]).toEqual({
    case: 'range',
    value: {
      phase: ActionPhase.RELEASED,
      handle: 'favored',
      values: [0, 1, 1],
    },
  })
  expect(inputCommand('arena', 'delay', changes[changes.length - 1]!)).toBe(
    'modlockinput arena delay range:2:favored:0,1,1',
  )
  expect(panel.children.some((child) => child.id === 'RangeGhost')).toBe(false)
  expect(drop(favored!)).toBe(false)

  // A stack of knobs gives up the one that can move the way the pointer went.
  drawing.draw(delay([0, 0, 1]))
  expect(shown()).toEqual(['0.000%', '0.000%', '100.000%'])
  point(0.5)
  expect(grab(low!)).toBe(true)
  expect(changes[changes.length - 1]).toEqual({
    case: 'range',
    value: { phase: ActionPhase.PRESSED, handle: 'favored', values: [] },
  })
  expect(shown()).toEqual(['0.000%', '50.000%', '100.000%'])

  // A stock overlay cancels the hold and puts the mod's values back.
  stock = 'other'
  runScheduled()
  expect(changes[changes.length - 1]).toEqual({
    case: 'range',
    value: { phase: ActionPhase.CANCELLED, handle: 'favored', values: [] },
  })
  expect(shown()).toEqual(['0.000%', '0.000%', '100.000%'])
  const sent = changes.length
  expect(drop(low!)).toBe(false)

  // Under the overlay a knob cannot be grabbed.
  expect(grab(high!)).toBe(false)
  stock = null

  // A control that goes mid-drag reports nothing, and drawn again shows the
  // mod's values on a new panel.
  expect(grab(high!)).toBe(true)
  drawing.draw(screen({ id: '' }))
  runScheduled()
  expect(drop(high!)).toBe(false)
  expect(changes.length).toBe(sent + 1)
  drawing.draw(delay([0.25, 0.5, 0.75]))
  panel = root.children[0]!.children[0]!
  expect(knobs(panel)[2]).not.toBe(high)
  expect(shown()).toEqual(['25.000%', '50.000%', '75.000%'])

  // Disabling the control cancels a hold, and a disabled knob holds still.
  expect(grab(knobs(panel)[0]!)).toBe(true)
  drawing.draw(delay([0.25, 0.5, 0.75], true))
  expect(changes[changes.length - 1]).toEqual({
    case: 'range',
    value: { phase: ActionPhase.CANCELLED, handle: 'min', values: [] },
  })
  expect(grab(built[0]!)).toBe(false)

  // Left and right step the active handle by the track's step and report
  // every value at once, stopping at its neighbor.
  drawing.draw(delay([0.25, 0.5, 0.75]))
  const press = (event: string) => panel.events.get(event)!()
  press('onfocus')
  expect(built[0]!.style.border).toBe('2px solid #ffffff')
  press('onmoveright')
  expect(changes[changes.length - 1]).toEqual({
    case: 'range',
    value: {
      phase: ActionPhase.RELEASED,
      handle: 'min',
      values: [0.5, 0.5, 0.75],
    },
  })
  const stepped = changes.length
  press('onmoveright')
  expect(changes.length).toBe(stepped)
  expect(shown()).toEqual(['50.000%', '50.000%', '75.000%'])

  // Confirm makes the next handle active, which stops at the track's end.
  press('onactivate')
  press('onactivate')
  expect(built[0]!.style.border).toBe(null)
  expect(built[2]!.style.border).toBe('2px solid #ffffff')
  press('onmoveright')
  press('onmoveright')
  expect(changes.length).toBe(stepped + 1)
  expect(changes[changes.length - 1]).toEqual({
    case: 'range',
    value: {
      phase: ActionPhase.RELEASED,
      handle: 'max',
      values: [0.5, 0.5, 1],
    },
  })

  // A redraw of the mod's old values leaves the step shown, and a stock
  // overlay or a disabled control takes no step.
  drawing.draw(delay([0.25, 0.5, 0.75]))
  expect(shown()).toEqual(['50.000%', '50.000%', '100.000%'])
  stock = 'other'
  press('onmoveleft')
  stock = null
  drawing.draw(delay([0.5, 0.5, 1], true))
  press('onmoveleft')
  expect(changes.length).toBe(stepped + 1)
  press('onblur')
  expect(built[2]!.style.border).toBe(null)
})

test('sliders map native fractions to ranges and send scoped wheel anchors', () => {
  // Set up a renderer that reports the values it sends.
  const root = new FakePanel('Panel', 'ModlockUi')
  const changes: InputValue[] = []
  const drawing = new Drawing(
    $,
    root,
    () => {},
    undefined,
    (_mod, _node, value) => changes.push(value),
  )
  const timeline = (
    value: number,
    min = 0,
    max = 323.3,
    step = 0,
    wheel = true,
    disabled = false,
  ) =>
    screen(
      { id: '', children: ['time'] },
      {
        id: 'time',
        kind: 'KIND_INPUT',
        disabled,
        slider: { value, min, max, step, wheel },
      },
    )

  // The native position is normalized even when the mod works in seconds.
  drawing.draw(timeline(3.5))
  let panel = root.children[0]!.children[0]!
  expect(panel.value).toBeCloseTo(3.5 / 323.3)
  panel.value = 0.5
  panel.events.get('onvaluechanged')!()
  runScheduled()
  expect(changes[changes.length - 1]).toEqual({
    case: 'number',
    value: Math.fround(161.65),
  })

  // A wheel event carries the pointer's share of the slider, without a seek:
  // the pointer at window pixel 200 is halfway along a slider 200 pixels wide
  // from pixel 100.
  const before = panel.value
  expect(handlers.get(panel)!.get('ScrollUp')!()).toBe(true)
  expect(changes[changes.length - 1]).toEqual({
    case: 'wheel',
    value: { delta: 1, fraction: 0.5 },
  })
  expect(panel.value).toBe(before)
  expect(inputCommand('arena', 'time', changes[changes.length - 1]!)).toBe(
    'modlockinput arena time wheel:1:0.5',
  )

  // Leave a seek awaiting the mod's echo.
  panel.value = 100 / 323.3
  panel.events.get('onvaluechanged')!()
  runScheduled()
  expect(changes[changes.length - 1]).toEqual({ case: 'number', value: 100 })

  // Zoom around the echoed value without replacing the native panel.
  const beforeZoom = panel
  drawing.draw(timeline(100, 90, 110))
  panel = root.children[0]!.children[0]!
  expect(panel).toBe(beforeZoom)
  expect(panel.value).toBe(0.5)

  // The next edit must use the new range.
  panel.value = 0.75
  panel.events.get('onvaluechanged')!()
  runScheduled()
  expect(changes[changes.length - 1]).toEqual({ case: 'number', value: 105 })

  // Changed bounds use the current range and snap relative to its minimum.
  drawing.draw(timeline(-5, -10, 10, 2.5))
  panel = root.children[0]!.children[0]!
  expect(panel.value).toBe(0.25)
  panel.value = 0.31
  panel.events.get('onvaluechanged')!()
  runScheduled()
  expect(changes[changes.length - 1]).toEqual({ case: 'number', value: -5 })

  // Disabled, opted-out and removed sliders cannot produce wheel input.
  drawing.draw(timeline(0, -10, 10, 2.5, false))
  panel = root.children[0]!.children[0]!
  expect(handlers.get(panel)!.get('ScrollUp')!()).toBe(false)
  drawing.draw(timeline(0, -10, 10, 2.5, true, true))
  expect(handlers.get(panel)!.get('ScrollDown')!()).toBe(false)
  drawing.draw(screen({ id: '' }))
  expect(handlers.get(panel)!.get('ScrollUp')!()).toBe(false)
})

test('eases panels in, cycles them and eases them out', () => {
  // Draw a panel that fades in, pulses twice and fades out.
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  const pulse: JsonObject = {
    id: 'pulse',
    style: { opacity: 1, transition: { seconds: 0.2 } },
    enter: { opacity: 0 },
    exit: { opacity: 0 },
    cycle: { style: { opacity: 0.5 }, seconds: 0.4, count: 1 },
    children: ['word'],
  }
  const word = { id: 'word', kind: 'KIND_LABEL', text: 'Go' }
  drawing.draw(screen({ id: '', children: ['pulse'] }, pulse, word))
  const panel = root.children[0]!.children[0]!
  const label = panel.children[0]!

  // It starts at its enter style, then swings to its cycle's far end and back.
  expect(panel.style.opacity).toBe('0')
  runScheduled()
  expect(panel.style.opacity).toBe('0.5')
  runScheduled()
  expect(panel.style.opacity).toBe('1')
  runScheduled()
  expect(scheduled.size).toBe(0)

  // Leaving the tree eases it to its exit style before its panel goes, and
  // its label goes with it.
  drawing.draw(screen({ id: '' }))
  expect(panel.style.opacity).toBe('0')
  expect(panel.hittest).toBe(false)
  expect(panel.deleteSeconds).toBeCloseTo(0.2)
  expect(label.deleteSeconds).toBe(-1)
})

test('frees the cursor a root asks for and hands the keyboard back when it captures it', () => {
  expect(cursorFreed(null)).toBe(false)
  expect(cursorAsks(null)).toBe(0)
  const asking = screen({ id: '', freeCursor: true, freeCursorAsks: 2 })
  expect(cursorFreed(asking)).toBe(true)
  expect(cursorAsks(asking)).toBe(2)
  expect(cursorAsks(screen({ id: '' }))).toBe(0)

  // The cursor starts captured, so a screen that frees nothing sends nothing.
  const sent: unknown[][] = []
  const hud = new FakePanel('CitadelHud', 'Hud')
  const cursor = new Cursor((...event) => sent.push(event), hud)
  cursor.set(false, 0)
  expect(sent).toEqual([])

  // A root that frees the cursor frees it once, and again when its mod asks
  // again after the player put it away.
  cursor.set(true, 1)
  cursor.set(true, 1)
  cursor.set(true, 2)
  expect(sent).toEqual([
    ['CitadelConCommand', 'hud_free_cursor 1'],
    ['CitadelConCommand', 'hud_free_cursor 1'],
  ])

  // Capturing it drops the focus a clicked panel holds in the HUD before the
  // game takes the cursor, so movement and the camera answer at once.
  sent.length = 0
  cursor.set(false, 0)
  expect(sent).toEqual([
    ['DropInputFocus', hud],
    ['CitadelConCommand', 'hud_free_cursor 0'],
  ])
})

/** gameHud builds the game's HUD with a panel for each part a test hides. */
test("hides a node that hides in chat while the game's chat box is open", () => {
  // Draw a lane that hides in chat beside a label that does not, in a HUD
  // whose chat box is closed.
  const game = new FakePanel('CitadelHud', 'Hud')
  const box = $.CreatePanel('CitadelChat', game, 'Chat') as unknown as FakePanel
  const root = $.CreatePanel('Panel', game, 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  const lane = { id: 'lane', hideInChat: true, children: ['line'] }
  const line = { id: 'line', kind: 'KIND_LABEL', text: 'over here' }
  const score = { id: 'score', kind: 'KIND_LABEL', text: '3' }
  drawing.draw(
    screen({ id: '', children: ['lane', 'score'] }, lane, line, score),
  )
  const [lanePanel, scorePanel] = (root as unknown as FakePanel).children[0]!
    .children
  expect(lanePanel!.visible).toBe(true)

  // The player opens the chat box: the lane hides at the next read, the
  // label stays, and a line drawn meanwhile stays hidden in it.
  box.classes.push(CHAT_OPEN)
  runScheduled()
  expect(lanePanel!.visible).toBe(false)
  expect(scorePanel!.visible).toBe(true)
  drawing.draw(
    screen({ id: '', children: ['lane', 'score'] }, lane, line, score),
  )
  expect(lanePanel!.visible).toBe(false)

  // Closing it shows the lane again; once the lane leaves, nothing is read.
  box.classes.length = 0
  runScheduled()
  expect(lanePanel!.visible).toBe(true)
  drawing.draw(screen({ id: '', children: ['score'] }, score))
  runScheduled()
  expect(scheduled.size).toBe(0)
})

function gameHud() {
  const game = new FakePanel('CitadelHud', 'Hud')
  const add = (parent: FakePanel, id: string, ...classes: string[]) => {
    const panel = $.CreatePanel('Panel', parent, id) as unknown as FakePanel
    panel.classes = classes
    return panel
  }

  // Lay out the panels the parts name, and one that stays.
  const health = add(game, 'health_and_abilities_container')
  const bars = add(health, 'hud_health_bars')
  const shields = add(health, 'HudShieldsContainer')
  const held = add(health, 'hud_held')
  const announcements = add(game, 'CitadelHudGameAnnouncements')
  const boon = add(announcements, '', 'LevelUpBlock', 'AbilityUnlockBlock')

  // Lay out the top bar, whose chat stays.
  const topBar = add(game, 'TopBar')
  const scores = add(topBar, 'TeamScores')
  const chat = add(topBar, 'Chat', 'ChatContainer')

  // Lay out the ammo at the crosshair.
  const crosshair = add(game, 'crosshair')
  const clip = add(crosshair, 'ammo_panel')
  const lowAmmo = add(game, 'low_ammo_label')

  // Lay out the souls, the minimap, the abilities, the stats and the notices.
  const souls = add(game, 'gold_and_ap_container')
  const minimap = add(game, 'minimap_persp')
  const abilities = add(game, 'AbilitiesContainer')
  const stats = add(game, 'hudPlayerStats')
  const castFailed = add(game, 'cast_failed_box')
  const indicators = add(game, 'HudEventIndicatorsPanel')
  return {
    game,
    add,
    health,
    bars,
    shields,
    held,
    announcements,
    boon,
    scores,
    chat,
    crosshair,
    clip,
    lowAmmo,
    souls,
    minimap,
    abilities,
    stats,
    castFailed,
    indicators,
  }
}

/** clock is a guard's host on a fake clock, ticking through the fake $.Schedule. */
function clock() {
  const host = {
    time: 1000,
    Schedule: (_: number, callback: () => void) => {
      $.Schedule(0, callback)
    },
    now: () => host.time,
    // Each panel keeps every handler put on it, as a dead script's stay.
    listeners: new Map<GuardPanel, (() => void)[]>(),
    listen: (panel: GuardPanel, handler: () => void) => {
      host.listeners.set(panel, [...(host.listeners.get(panel) ?? []), handler])
    },
    pressed: [] as string[],
    press: (mod: string, node: string) => {
      host.pressed.push(`${mod} ${node}`)
    },
    /** click runs every handler on panel, as one Activated event does. */
    click: (panel: GuardPanel) => {
      for (const handler of host.listeners.get(panel) ?? []) handler()
    },
    create: (
      type: string,
      parent: GuardPanel,
      id: string,
      properties?: Record<string, string>,
    ) =>
      (properties
        ? $.CreatePanel(type, parent as unknown as Panel, id, properties)
        : $.CreatePanel(
            type,
            parent as unknown as Panel,
            id,
          )) as unknown as GuardPanel,
    // A dialog variable reads the panel's; a hero's name reads heroNames.
    // The game's $.Localize throws on a second argument that is not a panel.
    localize: (...args: [string, GuardPanel?]) => {
      const [token, panel] = args
      if (args.length > 1 && !panel)
        throw new Error(
          "Localize() optional panel argument present but isn't a panel",
        )
      const variable = /^\{s:(.*)\}$/.exec(token)
      if (variable)
        return (panel as FakePanel | undefined)?.dialog[variable[1]!] ?? token
      return heroNames[token.slice(1)] ?? token
    },
    events: [] as unknown[][],
    dispatch: (event: string, ...values: unknown[]) => {
      host.events.push([event, ...values])
    },
  }
  return host
}

/** heroNames holds the heroes' names as the game's language shows them. */
const heroNames: Record<string, string> = {
  hero_atlas: 'Abrams',
  hero_bebop: 'Bebop',
}

/** parts reads the parts a screen whose one root hides names claims. */
function parts(...names: string[]) {
  return hiddenParts(screen({ id: '', hideHud: names }))
}

test("hides the game's HUD parts while a live screen claims them", () => {
  const hud = gameHud()
  const host = clock()
  const stock = new StockGuard(hud.game, host)

  // Hiding the player's health hides its panels, each marked to go back to
  // its sheets, and keeps the rest.
  stock.screen(parts('HUD_PART_OWN_HEALTH'))
  expect(hud.bars.visible).toBe(false)
  expect(hud.bars.attributes.get(HIDDEN_MARK)).toBe('sheets')
  expect(hud.shields.visible).toBe(false)
  expect(hud.held.visible).toBe(true)
  expect(hud.boon.visible).toBe(true)

  // A level-up notice the game shows later hides on the next tick.
  stock.screen(parts('HUD_PART_OWN_HEALTH', 'HUD_PART_LEVEL_UP'))
  expect(hud.boon.visible).toBe(false)
  expect(hud.scores.visible).toBe(true)
  const later = hud.add(
    hud.announcements,
    '',
    'LevelUpBlock',
    'PowerIncreaseBlock',
  )
  expect(scheduled.size).toBe(1)
  runScheduled()
  expect(later.visible).toBe(false)

  // The top bar hides all but its chat.
  stock.screen(
    parts('HUD_PART_OWN_HEALTH', 'HUD_PART_LEVEL_UP', 'HUD_PART_TOP_BAR'),
  )
  expect(hud.scores.visible).toBe(false)
  expect(hud.chat.visible).toBe(true)

  // Ammo hides only its own panels, not the crosshair, and the parts
  // dropped show again.
  stock.screen(parts('HUD_PART_LEVEL_UP', 'HUD_PART_AMMO'))
  expect(hud.clip.visible).toBe(false)
  expect(hud.lowAmmo.visible).toBe(false)
  expect(hud.crosshair.visible).toBe(true)
  expect(hud.scores.visible).toBe(true)
  expect(hud.scores.attributes.get(HIDDEN_MARK)).toBe('')
  expect(hud.bars.visible).toBe(true)

  // The souls, minimap, abilities, stats and notices hide whole.
  stock.screen(
    parts(
      'HUD_PART_SOULS',
      'HUD_PART_MINIMAP',
      'HUD_PART_ABILITIES',
      'HUD_PART_STATS',
      'HUD_PART_CAST_FAILED',
      'HUD_PART_EVENT_INDICATORS',
    ),
  )
  const whole = [
    hud.souls,
    hud.minimap,
    hud.abilities,
    hud.stats,
    hud.castFailed,
    hud.indicators,
  ]
  for (const panel of whole) expect(panel.visible).toBe(false)
  expect(hud.health.visible).toBe(true)

  // Status indicators hide only the crowd control ones, and one the game
  // floats later hides on the next tick.
  const stun = hud.add(hud.indicators, '', 'WindowRoot', 'Stunned')
  const damage = hud.add(hud.indicators, '', 'WindowRoot', 'cumulative')
  stock.screen(parts('HUD_PART_STATUS_INDICATORS'))
  expect(stun.visible).toBe(false)
  expect(damage.visible).toBe(true)
  expect(hud.indicators.visible).toBe(true)
  const silence = hud.add(hud.indicators, '', 'WindowRoot', 'Silenced')
  runScheduled()
  expect(silence.visible).toBe(false)

  // A relay that ended shows everything again, and the ticks stop.
  stock.screen(null)
  for (const panel of [...whole, hud.boon, later, hud.clip, stun, silence])
    expect(panel.visible).toBe(true)
  runScheduled()
  expect(scheduled.size).toBe(0)
})

/** abilityBar lays the ability bar under hud: four icons, each with its pips. */
function abilityBar(hud: ReturnType<typeof gameHud>) {
  const signature = hud.add(hud.abilities, 'hud_signature')
  const icons = [1, 2, 3, 4].map((n) =>
    hud.add(signature, `slot_signature_${n}`),
  )
  const pips = icons.map((icon) => {
    const row = new FakePanel('CitadelHudAbilityUpgradePips', '')
    row.parent = icon
    icon.children.push(row)
    return row
  })
  return { signature, icons, pips }
}

test('sends a click on the ability bar as a press only while allowed', () => {
  // A screen whose root asks for none puts no handler on the bar.
  const hud = gameHud()
  const bar = abilityBar(hud)
  const host = clock()
  const stock = new StockGuard(hud.game, host)
  stock.screen(parts())
  expect(host.listeners.size).toBe(0)

  // One that asks puts one on each icon and its pips, and a click, which
  // reaches both, is one press of the slot's node.
  stock.screen(
    parts(),
    abilityBarMods(screen({ id: '', abilityBarPresses: true })),
  )
  expect(host.listeners.size).toBe(8)
  host.click(bar.icons[1]!)
  host.click(bar.pips[1]!)
  expect(host.pressed).toEqual(['arena ability:1'])
  host.time += SAME_CLICK_SECONDS
  host.click(bar.pips[3]!)
  host.click(bar.pips[1]!)
  expect(host.pressed).toEqual([
    'arena ability:1',
    'arena ability:3',
    'arena ability:1',
  ])

  // The ticks keep the handlers, adding none twice, and put one on an icon
  // the game rebuilt.
  host.time += WALK_SECONDS
  runScheduled()
  expect(host.listeners.get(bar.icons[0]!)?.length).toBe(1)
  bar.icons[0]!.DeleteAsync(0)
  const rebuilt = hud.add(bar.signature, 'slot_signature_1')
  runScheduled()
  host.time += 1
  host.click(rebuilt)
  expect(host.pressed[host.pressed.length - 1]).toBe('arena ability:0')

  // A leave, the hideout, an ended relay and a lapsed lease each stop the
  // presses.
  const sent = () => host.pressed.length
  let before = sent()
  stock.leave()
  host.time += 1
  host.click(bar.icons[2]!)
  expect(sent()).toBe(before)
  stock.screen(parts(), ['arena'])
  hud.game.classes.push(CONNECTED_TO_HIDEOUT)
  host.click(bar.icons[2]!)
  expect(sent()).toBe(before)
  hud.game.classes.length = 0
  host.click(bar.icons[2]!)
  expect(sent()).toBe(++before)
  host.time += LEASE_SECONDS
  host.click(bar.icons[3]!)
  expect(sent()).toBe(before)
  stock.screen(null)
  host.time += 1
  host.click(bar.icons[3]!)
  expect(sent()).toBe(before)

  // With no claim and no bar asked, the ticks stop.
  runScheduled()
  runScheduled()
  expect(scheduled.size).toBe(0)
})

test('a handler an earlier script left on the bar sends nothing', () => {
  // The first guard's handlers stay on the bar after its script ends.
  const hud = gameHud()
  const bar = abilityBar(hud)
  const host = clock()
  const dead = new StockGuard(hud.game, host)
  dead.screen(parts(), ['arena'])
  const token = hud.game.GetAttributeString(BAR_GATE, '')
  expect(token).not.toBe('')

  // A guard that starts again takes the gate, and while it asks for nothing
  // the dead handler, whose guard still counts itself allowed, sends nothing.
  scheduled.clear()
  host.time += 0.01
  const live = new StockGuard(hud.game, host)
  expect(hud.game.GetAttributeString(BAR_GATE, '')).not.toBe(token)
  host.click(bar.icons[0]!)
  expect(host.pressed).toEqual([])

  // Once it asks, a click is one press, from its own handler.
  live.screen(parts(), ['arena'])
  expect(host.listeners.get(bar.icons[0]!)?.length).toBe(2)
  host.click(bar.icons[0]!)
  expect(host.pressed).toEqual(['arena ability:0'])
})

test('hands a restored panel back to the game', () => {
  // A panel the game shows by class follows the class after a restore: the
  // restore leaves no inline value behind, so a class removed later hides
  // it. A visible = true restore would pin it shown.
  const hud = gameHud()
  const stock = new StockGuard(hud.game, clock())
  stock.screen(parts('HUD_PART_MINIMAP', 'HUD_PART_SOULS'))
  expect(hud.minimap.visible).toBe(false)
  stock.screen([])
  expect(hud.minimap.visible).toBe(true)
  expect(hud.minimap.style.visibility).toBeNull()
  expect(hud.minimap.attributes.get(HIDDEN_MARK)).toBe('')
  hud.minimap.sheet = false
  expect(hud.minimap.visible).toBe(false)
  hud.minimap.sheet = true
  expect(hud.minimap.visible).toBe(true)

  // A panel the game hides by class, as gameplay_hud is until a team is
  // joined, stays hidden after the restore and shows when its class does.
  hud.souls.sheet = false
  stock.screen(parts('HUD_PART_SOULS'))
  stock.screen([])
  expect(hud.souls.visible).toBe(false)
  hud.souls.sheet = true
  expect(hud.souls.visible).toBe(true)
})

test('keeps a panel the game hid inline hidden after a restore', () => {
  // The game hides its hero shop by an inline collapse, which the whole
  // HUD's restore records and writes back.
  const hud = gameHud()
  const shop = hud.add(hud.game, 'CitadelHudHeroShop')
  shop.style.visibility = 'collapse'
  const stock = new StockGuard(hud.game, clock())
  stock.screen(parts('HUD_PART_ALL'))
  expect(shop.attributes.get(HIDDEN_MARK)).toBe('collapse')
  stock.screen([])
  expect(shop.style.visibility).toBe('collapse')
  expect(shop.attributes.get(HIDDEN_MARK)).toBe('')
})

test('hides again a panel another writer showed again', () => {
  // A write that shows a hidden panel again is undone on the next tick. On
  // a named part's panel it is a script's and goes at the restore; on any
  // other the game's write is recorded, and a newer one stands.
  const hud = gameHud()
  const shop = hud.add(hud.game, 'CitadelHudHeroShop')
  const stock = new StockGuard(hud.game, clock())
  stock.screen(parts('HUD_PART_ALL'))
  for (const panel of [hud.souls, shop]) panel.style.visibility = 'visible'
  runScheduled()
  expect(hud.souls.visible).toBe(false)
  expect(hud.souls.attributes.get(HIDDEN_MARK)).toBe('sheets')
  expect(shop.visible).toBe(false)
  expect(shop.attributes.get(HIDDEN_MARK)).toBe('visible')
  hud.minimap.style.visibility = 'visible'
  stock.screen([])
  expect(hud.souls.style.visibility).toBeNull()
  expect(hud.minimap.style.visibility).toBeNull()
  expect(shop.style.visibility).toBe('visible')
})

test('gives a named part back to its sheets whatever a script left on it', () => {
  // A script hid the top bar's scores with visible false and pinned its
  // kill counter, which the game shows on Tab by class, with visible true;
  // after the guard hides and restores the bar, both draw by their classes.
  for (const part of ['HUD_PART_TOP_BAR', 'HUD_PART_ALL']) {
    const hud = gameHud()
    const bar = hud.scores.GetParent()!
    const kills = hud.add(bar, '', 'KillsAndAbilityPointsContainer')
    kills.sheet = false
    hud.scores.visible = false
    kills.visible = true
    const stock = new StockGuard(hud.game, clock())
    stock.screen(parts(part))
    expect(hud.scores.visible).toBe(false)
    expect(kills.visible).toBe(false)
    stock.screen([])
    expect(hud.scores.style.visibility).toBeNull()
    expect(hud.scores.visible).toBe(true)
    expect(kills.style.visibility).toBeNull()
    expect(kills.visible).toBe(false)
    kills.sheet = true
    expect(kills.visible).toBe(true)
  }

  // A panel whose sheets name no visibility, as the top bar's portraits,
  // keeps what it last drew, so it draws again after the restore, also when
  // a script cleared the guard's collapse with null first; one the sheets
  // hide, as the street brawl box outside that mode, stays hidden.
  for (const cleared of [false, true]) {
    const hud = gameHud()
    const bar = hud.scores.GetParent()!
    const teams = hud.add(bar, 'TeamsContainer')
    const brawl = hud.add(bar, 'StretBrawlContainer')
    teams.sheet = null
    brawl.sheet = false
    const stock = new StockGuard(hud.game, clock())
    stock.screen(parts('HUD_PART_TOP_BAR'))
    expect(teams.visible).toBe(false)
    if (cleared) teams.style.visibility = null
    expect(teams.visible).toBe(false)
    stock.screen([])
    expect(teams.style.visibility).toBeNull()
    expect(teams.visible).toBe(true)
    expect(brawl.visible).toBe(false)
  }

  // A collapse an earlier release recorded on the bar goes too.
  const hud = gameHud()
  hud.scores.style.visibility = 'collapse'
  hud.scores.attributes.set(HIDDEN_MARK, 'collapse')
  new StockGuard(hud.game, clock())
  expect(hud.scores.visible).toBe(true)
  expect(hud.scores.attributes.get(HIDDEN_MARK)).toBe('')
})

test('gives the HUD back at once when the player leaves', () => {
  // A disconnect or shutdown restores the HUD without waiting for the lease.
  const hud = gameHud()
  const stock = new StockGuard(hud.game, clock())
  stock.screen(parts('HUD_PART_SOULS'))
  stock.leave()
  expect(hud.souls.visible).toBe(true)

  // A late screen read after the leave hides again only on the mod's
  // server; in the hideout it changes nothing.
  hud.game.classes.push(CONNECTED_TO_HIDEOUT)
  stock.screen(parts('HUD_PART_SOULS'))
  expect(hud.souls.visible).toBe(true)
  runScheduled()
  expect(hud.souls.visible).toBe(true)
})

test('gives the HUD back in the hideout without an event', () => {
  // The hideout class alone restores on the next tick.
  const hud = gameHud()
  const stock = new StockGuard(hud.game, clock())
  stock.screen(parts('HUD_PART_SOULS'))
  hud.game.classes.push(CONNECTED_TO_HIDEOUT)
  runScheduled()
  expect(hud.souls.visible).toBe(true)

  // The ticks stop with nothing hidden; back on the mod's server, the next
  // title brings the claim back.
  runScheduled()
  expect(scheduled.size).toBe(0)
  hud.game.classes.length = 0
  stock.alive()
  expect(hud.souls.visible).toBe(false)
})

test('gives the HUD back when the titles stop for the lease', () => {
  // A server that just goes away sends no event and no null; the lease runs
  // out instead, unless the relay's keepalive renews it.
  const hud = gameHud()
  const host = clock()
  const stock = new StockGuard(hud.game, host)
  stock.screen(parts('HUD_PART_SOULS'))
  host.time += LEASE_SECONDS - 1
  stock.alive()
  host.time += LEASE_SECONDS - 1
  runScheduled()
  expect(hud.souls.visible).toBe(false)
  host.time += 1
  runScheduled()
  expect(hud.souls.visible).toBe(true)

  // With nothing hidden the ticks stop, and a title brings the claim back.
  runScheduled()
  expect(scheduled.size).toBe(0)
  stock.alive()
  expect(hud.souls.visible).toBe(false)
})

test('shows what an earlier guard hid, without its memory', () => {
  // A reloaded script's guard restores every mark its predecessor left,
  // an earlier release's shown mark included, and leaves an unmarked
  // collapse, which is not the guard's.
  const hud = gameHud()
  new StockGuard(hud.game, clock()).screen(
    parts('HUD_PART_SOULS', 'HUD_PART_TOP_BAR'),
  )
  scheduled.clear()
  hud.minimap.style.visibility = 'collapse'
  hud.minimap.attributes.set(HIDDEN_MARK, 'shown')
  hud.clip.style.visibility = 'collapse'
  new StockGuard(hud.game, clock())
  expect(hud.souls.visible).toBe(true)
  expect(hud.scores.visible).toBe(true)
  expect(hud.souls.attributes.get(HIDDEN_MARK)).toBe('')
  expect(hud.minimap.style.visibility).toBeNull()
  expect(hud.clip.style.visibility).toBe('collapse')
  expect(scheduled.size).toBe(0)
})

test("hides the hero picker and the Broker's countdown", () => {
  // The picker sits in the game's HUD; the Broker's popup under the
  // interface's root, read only while the player stands at his booth.
  const hud = gameHud()
  const top = new FakePanel('Panel', 'Root')
  hud.game.parent = top
  top.children.push(hud.game)
  const picker = hud.add(hud.game, 'join_team')
  const actions = hud.add(hud.game, 'context_action_container')
  const popup = hud.add(top, 'ModsCorruptContainer')
  const countdown = hud.add(popup, 'Countdown')
  const stock = new StockGuard(hud.game, clock())

  // The picker hides at once, the countdown once the player is at the booth.
  stock.screen(parts('HUD_PART_HERO_PICKER', 'HUD_PART_BROKER_COUNTDOWN'))
  expect(picker.visible).toBe(false)
  expect(countdown.visible).toBe(true)
  actions.classes.push('in_corrupted_shop_zone')
  runScheduled()
  expect(countdown.visible).toBe(false)

  // Away from the booth the countdown still comes back when it is dropped.
  actions.classes.length = 0
  runScheduled()
  stock.screen([])
  expect(countdown.visible).toBe(true)
  expect(picker.visible).toBe(true)
})

test('hides the whole HUD but the interfaces and tooltips that stay', () => {
  // The HUD holds Modlock's root, the layout that reads the screens beside
  // one of the game's panels, Hyperline's panel, the game's tooltips, its
  // chat, and the top bar with the chat bubbles beside its clock.
  const hud = gameHud()
  const host = clock()
  const layouts = hud.add(hud.game, 'HudLayouts')
  const layout = hud.add(layouts, '')
  const inner = hud.add(layout, '')
  const beside = hud.add(layouts, 'GameLayout')
  const bar = hud.add(hud.game, 'TopBar')
  const clockPanel = hud.add(bar, 'GameClock')
  const bubbles = hud.add(bar, '')
  const stays = [
    hud.add(hud.game, 'ModlockUi'),
    hud.add(hud.game, 'HyperlineResident'),
    hud.add(hud.game, 'TooltipManager'),
    hud.add(hud.game, 'Chat'),
    hud.add(bubbles, 'Team1Chat'),
    hud.add(bubbles, 'Team2Chat'),
    layout,
  ]
  const stock = new StockGuard(hud.game, host, [layout])

  // Everything else hides, and the way down to the layout shows.
  stock.screen(parts('HUD_PART_ALL'))
  for (const panel of [
    hud.health,
    hud.announcements,
    hud.souls,
    beside,
    clockPanel,
  ])
    expect(panel.visible).toBe(false)
  for (const panel of [...stays, layouts, inner, bar, bubbles])
    expect(panel.visible).toBe(true)

  // A panel the game adds hides once the kept walk is old, not each tick.
  const added = hud.add(hud.game, 'Added')
  runScheduled()
  expect(added.visible).toBe(true)
  host.time += WALK_SECONDS
  runScheduled()
  expect(added.visible).toBe(false)

  // A kept panel the game removes walks again at once.
  const later = hud.add(hud.game, 'Later')
  hud.souls.DeleteAsync(0)
  runScheduled()
  expect(later.visible).toBe(false)

  // A panel that comes to hold one that stays shows again on the next walk.
  hud.add(hud.minimap, 'HyperlineLate')
  host.time += WALK_SECONDS
  runScheduled()
  expect(hud.minimap.visible).toBe(true)
  expect(hud.minimap.attributes.get(HIDDEN_MARK)).toBe('')

  // Dropped, it all comes back.
  stock.screen([])
  for (const panel of [hud.health, hud.announcements, beside, added, later])
    expect(panel.visible).toBe(true)
})

test('a reloaded guard finds every mark the whole HUD walk passes', () => {
  // A guard that hid the whole HUD marks it; a marked panel no finder names
  // any more, deep in the HUD, comes back, and the HUD's mark goes.
  const hud = gameHud()
  hud.game.attributes.set(ALL_MARK, '1')
  hud.held.visible = false
  hud.held.attributes.set(HIDDEN_MARK, 'shown')
  new StockGuard(hud.game, clock())
  expect(hud.held.visible).toBe(true)
  expect(hud.held.attributes.get(HIDDEN_MARK)).toBe('')
  expect(hud.game.attributes.get(ALL_MARK)).toBe('')
})

test('a restore walks the whole HUD only while a guard hides it', () => {
  // Count the reads of the HUD's children, which only a walk makes.
  const hud = gameHud()
  let walks = 0
  const count = hud.game.GetChildCount.bind(hud.game)
  hud.game.GetChildCount = () => (walks++, count())

  // With no mark on the HUD, a guard starting and a restore walk nothing.
  const parts = new HudParts(hud.game, () => 0)
  new StockGuard(hud.game, clock())
  parts.restoreAll()
  expect(walks).toBe(0)

  // Hiding the whole HUD marks it; giving it back walks and unmarks it, and
  // the next restore walks nothing.
  parts.hide(HudPart.ALL)
  expect(hud.game.attributes.get(ALL_MARK)).toBe('1')
  expect(hud.souls.visible).toBe(false)
  parts.restore(HudPart.ALL)
  expect(hud.souls.visible).toBe(true)
  expect(hud.game.attributes.get(ALL_MARK)).toBe('')
  walks = 0
  parts.restoreAll()
  expect(walks).toBe(0)

  // Hidden again at once, it walks again and hides a panel added since.
  const added = hud.add(hud.game, 'Added')
  parts.hide(HudPart.ALL)
  expect(added.visible).toBe(false)
})

test('loads layouts and style sheets a host allows and hands them data', () => {
  // Draw with a host that loads only arena's layouts.
  const root = new FakePanel('Panel', 'ModlockUi')
  let unloaded = 0
  const drawing = new Drawing($, root, () => {}, {
    url: (mod, path) => (mod === 'arena' ? `file://arena/${path}` : null),
    addon: (mod) => (mod === 'arena' ? 'arena' : null),
    unloaded: () => unloaded++,
  })
  const layout = (path: string, data: string) =>
    screen(
      { id: '', children: ['a'] },
      { id: 'a', kind: 'KIND_LAYOUT', layout: path, data, children: ['b'] },
      { id: 'b', kind: 'KIND_LABEL' },
    )

  // The layout loads with its data, and its children come from its file.
  drawing.draw(layout('shop.xml', '{"gold":1}'))
  const panel = root.children[0]!.children[0]!
  expect(panel.layout).toBe('file://arena/shop.xml')
  expect(panel.attributes.get('modlock_data')).toBe('{"gold":1}')
  expect(panel.attributes.get('modlock_mod')).toBe('arena')
  expect(panel.attributes.get('modlock_node')).toBe('a')
  expect(panel.children).toEqual([])
  expect(dispatched).toEqual([])

  // New data reaches the loaded layout and announces itself.
  drawing.draw(layout('shop.xml', '{"gold":2}'))
  expect(root.children[0]!.children[0]).toBe(panel)
  expect(panel.attributes.get('modlock_data')).toBe('{"gold":2}')
  expect(dispatched).toEqual([['PlaySoundEffect', layoutDataChanged]])

  // A reload replaces the layout even when its nodes and URL match, then
  // ordinary draws retain only that replacement.
  const beforeReload = root.children[0]!
  const reloaded = layout('shop.xml', '{"gold":2}')
  reloaded.trees![0]!.generation = 2n
  drawing.draw(reloaded)
  expect(beforeReload.valid).toBe(false)
  const replacementRoot = root.children[0]!
  const replacement = root.children[0]!.children[0]!
  expect(replacement).not.toBe(panel)
  expect(replacement.layout).toBe('file://arena/shop.xml')
  expect(root.children).toHaveLength(1)
  drawing.draw(reloaded)
  expect(root.children[0]!.children[0]).toBe(replacement)
  expect(unloaded).toBe(1)

  // A layout that failed to load tries again on the next draw.
  drawing.draw(layout('missing.xml', ''))
  expect(replacementRoot.valid).toBe(false)
  expect(unloaded).toBe(2)
  const missing = root.children[0]!.children[0]!
  drawing.draw(layout('missing.xml', ''))
  expect(root.children[0]!.children[0]).toBe(missing)
  expect(missing.layout).toBe('file://arena/missing.xml')

  // A path leaving the addon and a host that refuses the mod both draw an
  // empty panel.
  drawing.draw(layout('../session.xml', ''))
  expect(root.children[0]!.children[0]!.layout).toBe('')
  messages.length = 0
  new Drawing($, root, () => {}).draw(layout('shop.xml', ''))
  expect(root.children[root.children.length - 1]!.children[0]!.layout).toBe('')
  expect(messages).toEqual([
    '[Modlock] arena layout shop.xml has no content to load',
  ])

  // Modlock's own layouts load a mod's layout from the addon its tree names
  // and log once for a mod whose tree names none.
  const own = new FakePanel('Panel', 'ModlockUi')
  const addons = new AddonLayouts()
  const named = (mod: string, addon: string) =>
    Screen.fromJson({
      trees: [
        {
          mod,
          addon,
          nodes: [
            { id: '', children: ['a'] },
            { id: 'a', kind: 'KIND_LAYOUT', layout: 'shop.xml' },
          ],
        },
      ],
    })
  const installed = new Drawing($, own, () => {}, addons)
  const arena = named('arena', 'arena-0123abcd')
  addons.follow(arena)
  installed.draw(arena)
  expect(own.children[0]!.children[0]!.layout).toBe(
    'file://{resources}/layout/arena-0123abcd/shop.xml',
  )
  const bare = named('bare', '')
  messages.length = 0
  for (let times = 0; times < 2; times++) {
    addons.follow(bare)
    installed.draw(bare)
  }
  expect(messages).toEqual([
    '[Modlock] bare layout shop.xml has no content to load',
  ])

  // Removing the tree deletes the layout's panel and lets the host rebind.
  drawing.draw(layout('shop.xml', ''))
  unloaded = 0
  drawing.draw(null)
  expect(unloaded).toBe(1)

  // Draw a tree with style sheets through a host that loads layouts.
  const sheeted = new FakePanel('Panel', 'ModlockUi')
  const styled = new Drawing($, sheeted, () => {}, {
    url: (mod, path) => `file://${mod}/${path}`,
    addon: (mod) => mod,
    unloaded: () => {},
  })
  const sheets = (list: string[], classes: string[], flow?: string) =>
    screen(
      { id: '', sheets: list, children: ['c'], style: flow ? { flow } : {} },
      { id: 'c', kind: 'KIND_LABEL', classes },
    )

  // The root loads its first sheet into itself and each later one into a
  // panel nested in the one before, where its children hang; a sheet that
  // fails to load and a path leaving the addon leave the tree drawn.
  styled.draw(
    sheets(['hud.css', 'missing.css', '../x.css'], ['Score', 'Score']),
  )
  const modRoot = sheeted.children[0]!
  const nested = modRoot.children[0]!
  const label = nested.children[0]!

  // The nested panel covers the root, lets presses through and holds the label.
  expect(modRoot.layout).toBe('file://arena/hud.css.xml')
  expect([nested.id, nested.layout, nested.hittest]).toEqual([
    'ModlockUiSheet',
    'file://arena/missing.css.xml',
    false,
  ])
  expect(nested.style.width).toBe('100%')
  expect(nested.children).toEqual([label])
  expect(label.classes).toEqual(['Score'])

  // A class change adds and removes only what changed, on the same panel, and
  // the nested panel lays children out as the root does.
  styled.draw(
    sheets(['hud.css', 'missing.css'], ['Score', 'Leading'], 'FLOW_DOWN'),
  )
  styled.draw(sheets(['hud.css', 'missing.css'], ['Leading'], 'FLOW_DOWN'))
  expect(sheeted.children[0]!.children[0]!.children[0]).toBe(label)
  expect(label.classChanges).toEqual(['+Score', '+Leading', '-Score'])
  expect(nested.style.flowChildren).toBe('down')

  // A new list of sheets makes the tree again, and a host that loads no
  // layouts loads no sheets.
  styled.draw(sheets(['hud.css'], ['Leading']))
  expect(sheeted.children[0]!.children[0]!.paneltype).toBe('Label')
  const bareRoot = new FakePanel('Panel', 'ModlockUi')
  new Drawing($, bareRoot, () => {}).draw(sheets(['hud.css'], []))
  expect(bareRoot.children[0]!.layout).toBe('')
})

test("binds a layout's named panels and hears only their current presses", () => {
  // The layout's file builds a button and a label with the author's text.
  const author = (panel: FakePanel) => {
    for (const [type, id] of [
      ['Button', 'buy'],
      ['Label', 'price'],
    ]) {
      const child = new FakePanel(type!, id!)
      child.text = `author ${id}`
      child.parent = panel
      panel.children.push(child)
    }
  }
  FakePanel.built = author
  try {
    // Draw the layout with three bindings, one naming no panel.
    const root = new FakePanel('Panel', 'ModlockUi')
    const presses: string[] = []
    const drawing = new Drawing(
      $,
      root,
      (mod, node, action) => presses.push(`${mod}:${node}:${action}`),
      {
        url: (_, path) => `file://arena/${path}`,
        addon: () => 'arena',
        unloaded: () => {},
      },
    )
    let data = ''
    const draw = (...bindings: JsonObject[]) =>
      drawing.draw(
        screen(
          { id: '', children: ['a'] },
          { id: 'a', kind: 'KIND_LAYOUT', layout: 'shop.xml', data, bindings },
        ),
      )
    draw(
      { panel: 'buy', visible: false, press: true },
      { panel: 'price', text: '300' },
      { panel: 'ghost', text: 'gone' },
    )
    const layout = root.children[0]!.children[0]!
    let [buy, price] = layout.children
    expect(buy!.visible).toBe(false)
    expect(price!.text).toBe('300')
    expect(buy!.text).toBe('author buy')
    expect(messages).toEqual(['[Modlock] arena shop.xml has no panel ghost'])

    // A press on the bound button reaches the mod by the panel's name.
    handlers.get(buy!)!.get('Activated')!()
    expect(presses).toEqual(['arena:a:buy'])

    // A change applies; a press the draw no longer binds stays here, and a
    // missing panel logs only once.
    draw(
      { panel: 'buy', visible: true },
      { panel: 'price', text: '400' },
      { panel: 'ghost', text: 'gone' },
    )
    expect(buy!.visible).toBe(true)
    expect(price!.text).toBe('400')
    handlers.get(buy!)!.get('Activated')!()
    expect(presses).toEqual(['arena:a:buy'])
    expect(messages).toHaveLength(1)

    // A binding the mod drops gives the panel back the author's text.
    draw({ panel: 'buy', press: true })
    expect(price!.text).toBe('author price')
    expect(buy!.style.visibility).toBe('visible')

    // When the script builds its panels anew as it reads new data, every
    // binding applies again in the same draw.
    const dispatch = $.DispatchEvent.bind($)
    $.DispatchEvent = (...args: Parameters<typeof dispatch>) => {
      while (layout.children.length > 0) layout.children[0]!.DeleteAsync(0)
      author(layout)
      dispatch(...args)
    }
    data = '{"gold":1}'
    draw({ panel: 'buy', press: true }, { panel: 'price', text: '500' })
    $.DispatchEvent = dispatch
    ;[buy, price] = layout.children
    expect(price!.text).toBe('500')
    handlers.get(buy!)!.get('Activated')!()
    expect(presses).toEqual(['arena:a:buy', 'arena:a:buy'])
  } finally {
    FakePanel.built = null
  }
})

test('a binding sets the same text, classes, style and image as a typed node', () => {
  // The layout's file builds a styled label and an image.
  FakePanel.built = (panel: FakePanel) => {
    const price = new FakePanel('Label', 'price')
    price.text = 'author price'
    price.classes.push('author')
    price.style.color = '#abcdef'
    const icon = new FakePanel('Image', 'icon')
    for (const child of [price, icon]) {
      child.parent = panel
      panel.children.push(child)
    }
  }
  try {
    // Draw the same properties through bindings and through typed nodes.
    const look = {
      token: '#shop.price',
      variables: { gold: '300' },
      classes: ['sold-out', 'author'],
      style: { color: 0xff0000ff, fontSize: 30 },
    }
    const image = 'file://{images}/hud/gold.png'
    const root = new FakePanel('Panel', 'ModlockUi')
    const drawing = new Drawing($, root, () => {}, {
      url: (_, path) => `file://arena/${path}`,
      addon: () => 'arena',
      unloaded: () => {},
    })
    const draw = (...bindings: JsonObject[]) =>
      drawing.draw(
        screen(
          { id: '', children: ['a', 'l', 'i'] },
          { id: 'a', kind: 'KIND_LAYOUT', layout: 'shop.xml', bindings },
          { id: 'l', kind: 'KIND_LABEL', ...look },
          { id: 'i', kind: 'KIND_IMAGE', image },
        ),
      )
    draw({ panel: 'price', ...look }, { panel: 'icon', image })

    // The bound panels hold what the typed ones do.
    const [layout, label, typed] = root.children[0]!.children
    const [price, icon] = layout!.children
    expect(price!.text).toBe(label!.text)
    expect(price!.dialog).toEqual({ gold: '300' })
    expect(price!.classes).toEqual(['author', 'sold-out'])
    expect(price!.style.color).toBe(label!.style.color)
    expect(price!.style.fontSize).toBe(label!.style.fontSize)
    expect(icon!.image).toBe(typed!.image)

    // Dropping the properties gives back the author's text, class and inline
    // style, and the image stays.
    draw({ panel: 'price' }, { panel: 'icon' })
    expect(price!.text).toBe('author price')
    expect(price!.classes).toEqual(['author'])
    expect(price!.style.color).toBe('#abcdef')
    expect(price!.style.fontSize).toBeNull()
    expect(icon!.image).toBe(image)
  } finally {
    FakePanel.built = null
  }
})

test("hands a mod's routes to each of its layouts", () => {
  // Draw one layout, with routes built the way the controller sends them.
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {}, {
    url: (mod, path) => `file://${mod}/${path}`,
    addon: (mod) => mod,
    unloaded: () => {},
  })
  const route = (name: string, data: string) => ({
    id: `route:${name}`,
    kind: 'KIND_ROUTE',
    route: name,
    data,
  })
  const shop = { id: 'a', kind: 'KIND_LAYOUT', layout: 'shop.xml' }

  // A layout loads with the routes the mod already sends, and a route draws
  // no panel.
  drawing.draw(
    screen({ id: '', children: ['a'] }, shop, route('scores', '{"red":3}')),
  )
  const panel = root.children[0]!.children[0]!
  expect(root.children[0]!.children).toEqual([panel])
  expect(panel.attributes.get('modlock_route_scores')).toBe('{"red":3}')
  expect(dispatched).toEqual([])

  // A changed route and a new one reach the loaded layout with one ping, an
  // unchanged draw sends none, and a route with a name no attribute holds
  // never arrives.
  const both = (red: number) =>
    screen(
      { id: '', children: ['a'] },
      shop,
      route('scores', `{"red":${red}}`),
      route('round', '2'),
      route('bad name', '1'),
    )
  drawing.draw(both(4))
  expect(panel.attributes.get('modlock_route_scores')).toBe('{"red":4}')
  expect(panel.attributes.get('modlock_route_round')).toBe('2')
  expect([...panel.attributes.keys()]).not.toContain('modlock_route_bad name')
  expect(dispatched).toEqual([['PlaySoundEffect', layoutDataChanged]])

  // Drawing the same routes again wakes no layout.
  dispatched.length = 0
  drawing.draw(both(4))
  expect(dispatched).toEqual([])

  // A route the mod stops sending empties, and a reset that drops the tree
  // forgets its routes, so the next layout starts without them.
  drawing.draw(screen({ id: '', children: ['a'] }, shop, route('round', '2')))
  expect(panel.attributes.get('modlock_route_scores')).toBe('')
  expect(dispatched).toEqual([['PlaySoundEffect', layoutDataChanged]])
  drawing.draw(null)
  drawing.draw(screen({ id: '', children: ['a'] }, shop))
  expect(
    root.children[0]!.children[0]!.attributes.has('modlock_route_round'),
  ).toBe(false)
})

test('notifies layouts before deleting them or releasing the screen', () => {
  // Record every layout marked unloading when PlaySoundEffect fires, while
  // both the panel and its parent are still valid.
  const root = new FakePanel('Panel', 'ModlockUi')
  const notices: FakePanel[] = []
  const tracked = new Set<FakePanel>()
  const seen = new Set<FakePanel>()
  const drawing = new Drawing(
    {
      ...$,
      DispatchEvent(event: string, ...values: unknown[]) {
        dispatched.push([event, ...values.map(String)])
        if (event !== 'PlaySoundEffect' || values[0] !== layoutDataChanged) {
          return
        }
        for (const panel of tracked) {
          if (
            !seen.has(panel) &&
            panel.GetAttributeString(layoutUnloadingAttribute, '') === '1'
          ) {
            expect(panel.valid).toBe(true)
            expect(panel.parent?.valid).toBe(true)
            seen.add(panel)
            notices.push(panel)
          }
        }
      },
    },
    root,
    () => {},
    {
      url: (_mod, path) => `file://arena/${path}`,
      addon: () => 'arena',
      unloaded: () => {},
    },
  )
  const tree = (path = 'shop.xml', sheets: string[] = []) =>
    screen(
      { id: '', sheets, children: ['box', 'keep'] },
      { id: 'box', children: ['shop'] },
      { id: 'shop', kind: 'KIND_LAYOUT', layout: path },
      { id: 'keep', kind: 'KIND_LAYOUT', layout: 'keep.xml' },
    )
  const layoutPanels = () => {
    const modRoot = root.children[0]!
    const holder =
      modRoot.children[0]?.id === 'ModlockUiSheet'
        ? modRoot.children[0]
        : modRoot
    const shop = holder.children[0]!.children[0]!
    const keep = holder.children[1]!
    tracked.add(shop)
    tracked.add(keep)
    return { shop, keep }
  }

  const expectNotified = (...panels: FakePanel[]) => {
    expect(notices).toHaveLength(panels.length)
    expect(new Set(notices)).toEqual(new Set(panels))
    notices.length = 0
  }

  // Removing an ancestor notifies the nested layout before deleting the box,
  // and leaves a sibling layout alone.
  drawing.draw(tree())
  const first = layoutPanels()
  drawing.draw(
    screen(
      { id: '', children: ['keep'] },
      { id: 'keep', kind: 'KIND_LAYOUT', layout: 'keep.xml' },
    ),
  )
  expectNotified(first.shop)
  expect(first.shop.parent?.valid).toBe(false)
  expect(first.keep.attributes.has(layoutUnloadingAttribute)).toBe(false)

  // Replacing a layout in place notifies the old one before creating the new.
  drawing.draw(tree('shop.xml'))
  const second = layoutPanels()
  drawing.draw(tree('other.xml'))
  const third = layoutPanels()
  expectNotified(second.shop)
  expect(third.shop).not.toBe(second.shop)
  expect(third.shop.attributes.has(layoutUnloadingAttribute)).toBe(false)

  // Replacing the root through style sheets notifies layouts nested under its
  // sheet panels once before the old root goes.
  dispatched.length = 0
  drawing.draw(tree('other.xml', ['a.css', 'b.css']))
  const sheeted = layoutPanels()
  expectNotified(third.shop, third.keep)
  expect(dispatched).toEqual([['PlaySoundEffect', layoutDataChanged]])
  dispatched.length = 0
  drawing.draw(tree('other.xml', ['a.css']))
  const unnested = layoutPanels()
  expectNotified(sheeted.shop, sheeted.keep)
  expect(dispatched).toEqual([['PlaySoundEffect', layoutDataChanged]])

  // Reloading a tree's generation notifies every layout in the old tree.
  const reloaded = tree('other.xml', ['a.css'])
  reloaded.trees![0]!.generation = 2n
  drawing.draw(reloaded)
  const afterReload = layoutPanels()
  expectNotified(unnested.shop, unnested.keep)

  // Dropping the mod notifies its layouts before deleting its root.
  drawing.draw(null)
  expectNotified(afterReload.shop, afterReload.keep)

  // Releasing on disconnect notifies once; repeating release or drawing null
  // after sends no extra wake.
  drawing.draw(tree())
  const released = layoutPanels()
  dispatched.length = 0
  drawing.release()
  drawing.release()
  drawing.draw(null)
  expectNotified(released.shop, released.keep)
  expect(dispatched).toEqual([['PlaySoundEffect', layoutDataChanged]])

  // Drawing the same screen again after release replaces the unloaded layouts
  // with fresh panels without sending another wake, and dropping them notifies
  // the replacements.
  drawing.draw(tree())
  const stale = layoutPanels()
  drawing.release()
  expectNotified(stale.shop, stale.keep)
  dispatched.length = 0
  drawing.draw(tree())
  const fresh = layoutPanels()
  expect(fresh.shop).not.toBe(stale.shop)
  expect(fresh.keep).not.toBe(stale.keep)
  expect(fresh.shop.attributes.has(layoutUnloadingAttribute)).toBe(false)
  expect(fresh.keep.attributes.has(layoutUnloadingAttribute)).toBe(false)
  expect(dispatched).toEqual([])
  drawing.draw(null)
  expectNotified(fresh.shop, fresh.keep)

  // A layout that failed to load gets no unload notice when removed.
  dispatched.length = 0
  drawing.draw(
    screen(
      { id: '', children: ['bad'] },
      { id: 'bad', kind: 'KIND_LAYOUT', layout: 'missing.xml' },
    ),
  )
  drawing.draw(null)
  expect(dispatched).toEqual([])
})

test('reads screens in acknowledged parts', () => {
  // Start a bridge on a fake state panel, which loads the bridge page.
  const state = new FakePanel('HTML', 'ModlockUiState')
  const shown: (Screen | null)[] = []
  new Bridge(state, 'https://relay/bridge.html', (screen) => shown.push(screen))
  expect(state.url).toBe('https://relay/bridge.html')
  expect(scheduled.size).toBe(1)
  const title = (text: string) =>
    handlers.get(state)!.get('HTMLTitle')!(state.id, text)

  // A screen in two parts shows once both arrive, each acknowledged. The
  // packing writes printable ASCII as itself and other bytes above U+0100,
  // as the controller does; the name is not ASCII, so the UTF-8 decoder runs.
  const bytes = Screen.toBinary(
    Screen.create({ trees: [{ mod: 'arène', nodes: [{ id: '' }] }] }),
  )
  let source = ''
  for (const byte of bytes)
    source += String.fromCharCode(
      byte > 0x20 && byte < 0x7f && byte !== 0x5c ? byte : 0x100 + byte,
    )
  const half = Math.floor(source.length / 2)
  const part = (id: number, offset: number, chunk: string) =>
    `modlock:frame:${id}:${offset}:${source.length}:3:${chunk}`

  // The ready title ends the page's load deadline.
  title('modlock:bridge-ready')
  expect(scheduled.size).toBe(0)

  // The first part is acknowledged but not shown.
  title(part(7, 0, source.slice(0, half)))
  expect(state.url).toBe('https://relay/bridge.html#1')
  expect(shown).toEqual([])

  // The second part shows the screen and flips the acknowledgment.
  title(part(7, half, source.slice(half)))
  expect(state.url).toBe('https://relay/bridge.html#0')
  expect(shown.length).toBe(1)
  expect(shown[0]?.trees?.[0]?.mod).toBe('arène')
  expect(decodeUtf8(new TextEncoder().encode('arène ✓'))).toBe('arène ✓')

  // A part out of order is dropped, and a closed stream clears the screen.
  title(part(8, half, source.slice(half)))
  expect(shown.length).toBe(1)
  title('')
  expect(shown).toEqual([shown[0]!, null])
})

test("acknowledges the relay's keepalive and reports it", () => {
  // The relay's alive line arrives as a JSON line part, which the bridge
  // acknowledges so the page reads on, and reports once.
  const state = new FakePanel('HTML', 'ModlockUiState')
  const shown: (Screen | null)[] = []
  let alive = 0
  new Bridge(
    state,
    'https://relay/bridge.html',
    (screen) => shown.push(screen),
    () => alive++,
  )
  const title = (text: string) =>
    handlers.get(state)!.get('HTMLTitle')!(state.id, text)
  const line = (id: number, chunk: string) =>
    `modlock:snapshot:${JSON.stringify({ id, offset: 0, total: chunk.length, chunk })}`
  title(line(1, 'alive'))
  expect(state.url).toBe('https://relay/bridge.html#1')
  expect(alive).toBe(1)

  // A repeated title is not read twice, a later keepalive is, and any other
  // line is acknowledged without a report.
  title(line(1, 'alive'))
  expect(alive).toBe(1)
  title(line(2, 'alive'))
  expect(alive).toBe(2)
  expect(state.url).toBe('https://relay/bridge.html#0')
  title(line(3, '%7B%7D'))
  expect(alive).toBe(2)
  expect(state.url).toBe('https://relay/bridge.html#1')
  expect(shown).toEqual([])
})

/**
 * abilityIcon lays one game ability icon under parent, as hud_ability_icon
 * does: the frame and ring, the picture, the cooldown shade and the
 * read-only pips.
 */
function abilityIcon(parent: FakePanel, id: string) {
  const add = (under: FakePanel, name: string, ...classes: string[]) => {
    const panel = new FakePanel('Panel', name)
    panel.parent = under
    under.children.push(panel)
    panel.classes = classes
    return panel
  }
  const icon = add(parent, id, 'ability_container')
  const button = add(icon, 'button_container')
  const frame = add(button, '', 'ability_bg')
  const ring = add(button, '', 'ability_ring')
  const images = add(button, '', 'image_container')
  const own = add(images, 'ability_image', 'ability_image')
  const shade = add(images, 'cooldown_mask')
  const readOnly = add(icon, 'ReadOnlyPipsContainer')
  const pips = [3, 2, 1].map((n) => add(readOnly, `Pip${n}`)).reverse()
  return { icon, frame, ring, own, shade, readOnly, pips }
}

/**
 * tabRows lays Tab's rows under the top bar: Ada's, a bot's showing its
 * hero, and Cy's, each with four ability icons.
 */
function tabRows(hud: ReturnType<typeof gameHud>) {
  const team = new FakePanel('CitadelHudTopBarTeam', '')
  team.parent = hud.scores
  hud.scores.children.push(team)
  const row = (name: string, hero: string) => {
    const panel = new FakePanel('CitadelHudTopBarPlayer', '')
    panel.parent = team
    team.children.push(panel)
    panel.dialog.hero_name = hero
    if (name !== '') panel.dialog.player_name = name
    const box = hud.add(panel, 'PlayerAbilitiesContainer')
    return [1, 2, 3, 4].map((n) => abilityIcon(box, `AbilityButton${n}`))
  }
  return {
    ada: row('Ada', 'Abrams'),
    bot: row('', 'Bebop'),
    cy: row('Cy', 'Abrams'),
  }
}

/** displayed is a screen whose root shows rows. */
function displayed(...rows: JsonObject[]) {
  return abilityRows(screen({ id: '', abilityDisplay: rows }))
}

test("dresses Tab's rows as the display names them, and gives them back", () => {
  // Ada's row matches by name and the bot's by hero; Cy, whose hero is
  // Abrams too, is named in no row and keeps the game's icons.
  const hud = gameHud()
  const tab = tabRows(hud)
  const host = clock()
  const stock = new StockGuard(hud.game, host)
  const rows = displayed(
    {
      player: 2,
      name: 'Ada',
      hero: 'hero_atlas',
      abilities: [
        { ability: 11, image: 'file://{images}/a.png', upgrades: 0b101 },
      ],
    },
    {
      player: 3,
      name: 'BotPlayer',
      hero: 'hero_bebop',
      abilities: [{ ability: 22 }],
    },
  )
  stock.screen(parts(), [], rows)

  // Ada's first icon shows the named picture over the game's, the ring of
  // its upgrades and its first and third read-only pips lit.
  const ada = tab.ada[0]!
  const picture = ada.icon.FindChildTraverse(DISPLAY_IMAGE)!
  expect(picture.image).toBe('file://{images}/a.png')
  expect(picture.GetParent()).toBe(ada.own.GetParent())
  expect(ada.own.style.opacity).toBe('0')
  expect(ada.ring.style.visibility).toBe('visible')
  expect(ada.pips.map((pip) => pip.style.backgroundColor)).toEqual([
    'black',
    '#69e799',
    'black',
  ])

  // The bot's icon keeps the game's picture, since none is named, but its
  // hover opens the game's window for the named ability as slot 3 holds it.
  const bot = tab.bot[0]!
  expect(bot.own.style.opacity ?? null).toBeNull()
  expect(bot.ring.style.visibility).toBe('collapse')
  const cover = bot.icon.FindChildTraverse(DISPLAY_COVER)!
  expect(cover.GetParent()).toBe(bot.icon)
  cover.events.get('onmouseover')!()
  expect(host.events).toEqual([
    ['CitadelShowAbilityDetailsTooltip', bot.icon, 22, 3],
  ])
  cover.events.get('onmouseout')!()
  expect(host.events[host.events.length - 1]).toEqual([
    'CitadelHideAbilityDetailsTooltip',
    bot.icon,
  ])

  // Slots no ability is named for, and Cy's row, are left to the game.
  for (const icon of [tab.ada[1]!, tab.bot[3]!, ...tab.cy]) {
    expect(icon.icon.FindChildTraverse(DISPLAY_COVER)).toBeNull()
    expect(icon.icon.attributes.get(DRAWN_MARK) ?? '').toBe('')
  }

  // A row that drops an ability gives that icon back.
  stock.screen(
    parts(),
    [],
    displayed({ player: 2, name: 'Ada', abilities: [] }),
  )
  expect(ada.icon.FindChildTraverse(DISPLAY_IMAGE)).toBeNull()
  expect(ada.own.style.opacity).toBeNull()
  expect(ada.ring.style.visibility).toBeNull()
  expect(ada.pips.map((pip) => pip.style.backgroundColor)).toEqual([
    null,
    null,
    null,
  ])

  // A leave gives back every icon, and a hover left on a cover does nothing.
  stock.screen(parts(), [], rows)
  const left = tab.bot[0]!.icon.FindChildTraverse(DISPLAY_COVER)!
  stock.leave()
  for (const icon of [...tab.ada, ...tab.bot]) {
    expect(icon.icon.FindChildTraverse(DISPLAY_IMAGE)).toBeNull()
    expect(icon.icon.FindChildTraverse(DISPLAY_COVER)).toBeNull()
    expect(icon.own.style.opacity ?? null).toBeNull()
    expect(icon.ring.style.visibility ?? null).toBeNull()
  }
  host.events.length = 0
  left.events.get('onmouseover')!()
  expect(host.events).toEqual([])

  // With nothing drawn and nothing hidden, the ticks stop.
  runScheduled()
  runScheduled()
  expect(scheduled.size).toBe(0)
})

test("dresses a Tab icon whose picture the game builds late, and silences the icon's hover", () => {
  // Ada's first icon has no picture of its own yet when the display first
  // draws it.
  const hud = gameHud()
  const tab = tabRows(hud)
  const ada = tab.ada[0]!
  const images = ada.own.parent!
  images.children = images.children.filter((child) => child !== ada.own)
  const host = clock()
  const stock = new StockGuard(hud.game, host)
  const rows = displayed({
    player: 2,
    name: 'Ada',
    abilities: [{ ability: 11, image: 'file://{images}/a.png' }],
  })
  stock.screen(parts(), [], rows)
  expect(ada.icon.FindChildTraverse(DISPLAY_IMAGE)).toBeNull()

  // Once the game builds it, the next draw lays the named picture beside it,
  // well within the time a look is kept.
  images.children.push(ada.own)
  host.time += 0.01
  stock.screen(parts(), [], rows)
  const picture = ada.icon.FindChildTraverse(DISPLAY_IMAGE)!
  expect(picture.image).toBe('file://{images}/a.png')
  expect(picture.GetParent()).toBe(images)
  expect(ada.own.style.opacity).toBe('0')

  // While dressed, the icon's own hover opens nothing, leaving the cover's
  // window alone; a leave gives the icon's hover back to the game.
  ada.icon.events.get('onmouseover')!()
  expect(host.events).toEqual([])
  stock.leave()
  expect(ada.icon.events.has('onmouseover')).toBe(false)
})

test('a guard that starts again undresses what an earlier one drew', () => {
  // The first guard's dressing stays on Tab and the bar after its script ends.
  const hud = gameHud()
  const tab = tabRows(hud)
  const bar = barIcons(hud)
  const host = clock()
  new StockGuard(hud.game, host).screen(
    parts(),
    [],
    displayed({
      player: 2,
      name: 'Ada',
      own: true,
      abilities: [
        { ability: 11, image: 'file://{images}/a.png', upgrades: 0b11 },
      ],
    }),
  )
  const cover = tab.ada[0]!.icon.FindChildTraverse(DISPLAY_COVER)!
  expect(bar[0]!.frame.style.backgroundImage).not.toBeNull()
  scheduled.clear()

  // The next guard gives it all back before any screen, and the old cover's
  // hover, were it still reached, opens nothing.
  host.time += 0.01
  new StockGuard(hud.game, host)
  expect(tab.ada[0]!.icon.FindChildTraverse(DISPLAY_IMAGE)).toBeNull()
  expect(tab.ada[0]!.own.style.opacity).toBeNull()
  expect(bar[0]!.frame.style.backgroundImage).toBeNull()
  expect(bar[0]!.ring.style.visibility).toBeNull()
  cover.events.get('onmouseover')!()
  expect(host.events).toEqual([])
})

/**
 * barIcons lays the ability bar's four icons under hud, as the game's
 * hud_signature does, each with its upgrade pips: inside the icon, or apart,
 * the four rows together in one panel of their own.
 */
function barIcons(hud: ReturnType<typeof gameHud>, apart = false) {
  const signature = hud.add(hud.abilities, 'hud_signature')
  const icons = [1, 2, 3, 4].map((n) =>
    abilityIcon(signature, `slot_signature_${n}`),
  )
  const rows = apart ? hud.add(signature, 'pip_rows') : null
  return icons.map((icon) => {
    const pips = new FakePanel('CitadelHudAbilityUpgradePips', '')
    const holder = rows ?? icon.icon
    pips.parent = holder
    holder.children.push(pips)
    const part = (under: FakePanel, id: string, ...classes: string[]) => {
      const panel = hud.add(under, id, ...classes)
      return panel
    }
    const train = part(pips, 'TrainButton')
    const row = part(pips, 'AbilityUpgradePips')
    const unlocks = [1, 2, 3].map((tier) => {
      const unlock = part(row, `AbilityUnlock${tier}`)
      return {
        unlock,
        flash: part(unlock, '', 'upgradeReadyFlash'),
        pip: part(unlock, '', 'abilityLevelPip'),
        cost: part(unlock, '', 'abilityUpgradeCostLabel'),
      }
    })
    return { ...icon, pips, train, row, unlocks }
  })
}

test("draws the viewer's own row on the ability bar, and gives it back", () => {
  // The viewer's row draws the bar by slot; another player's draws nothing.
  const hud = gameHud()
  const bar = barIcons(hud)
  const host = clock()
  const stock = new StockGuard(hud.game, host)
  stock.screen(
    parts(),
    [],
    displayed(
      { player: 3, name: 'Bo', abilities: [{ ability: 9, upgrades: 0b1111 }] },
      {
        player: 2,
        name: 'Ada',
        own: true,
        abilities: [
          { ability: 11, upgrades: 0b1011 },
          { ability: 12, upgrades: 0b1 },
        ],
      },
    ),
  )

  // Two upgrades draw the second tier's frame, ring and shade.
  const first = bar[0]!
  expect(first.frame.style.backgroundImage).toBe(
    'url("s2r://panorama/images/hud/core/ability_frame_passive_2_psd.vtex")',
  )
  expect(first.ring.style.visibility).toBe('visible')
  expect(first.shade.style.opacityMask).toBe(
    'url("s2r://panorama/images/hud/core/ability_frame_2.vsvg")',
  )

  // The pips show flat, lit by upgrade, without the cost or train button.
  expect(first.train.style.visibility).toBe('collapse')
  expect(first.row.style.visibility).toBe('visible')
  expect(first.unlocks.map(({ pip }) => pip.style.backgroundColor)).toEqual([
    '#69e799',
    'black',
    '#69e799',
  ])
  expect(first.unlocks[0]!.cost.style.visibility).toBe('collapse')
  expect(first.unlocks[0]!.flash.style.opacity).toBe('0')
  expect(first.unlocks[0]!.pip.style.height).toBe('6px')

  // An unlocked ability without upgrades has the plain frame and no ring;
  // a slot with no ability is the game's.
  expect(bar[1]!.frame.style.backgroundImage).toBe(
    'url("s2r://panorama/images/hud/core/ability_frame_passive_0_psd.vtex")',
  )
  expect(bar[1]!.ring.style.visibility).toBe('collapse')
  expect(bar[2]!.frame.style.backgroundImage ?? null).toBeNull()
  expect(bar[2]!.train.style.visibility ?? null).toBeNull()

  // Pips the game builds again are drawn at the next look.
  const rebuilt = new FakePanel('CitadelHudAbilityUpgradePips', '')
  first.icon.children.splice(first.icon.children.indexOf(first.pips), 1)
  first.pips.valid = false
  rebuilt.parent = first.icon
  first.icon.children.push(rebuilt)
  const train = hud.add(rebuilt, 'TrainButton')
  runScheduled()
  expect(train.style.visibility).toBe('collapse')

  // A relay that ended gives back every style, on the rebuilt pips too.
  stock.screen(null)
  expect(train.style.visibility).toBeNull()
  for (const slot of bar.slice(1)) {
    expect(slot.frame.style.backgroundImage ?? null).toBeNull()
    expect(slot.ring.style.visibility ?? null).toBeNull()
    expect(slot.shade.style.opacityMask ?? null).toBeNull()
    expect(slot.train.style.visibility ?? null).toBeNull()
    expect(slot.row.style.visibility ?? null).toBeNull()
    for (const { pip, cost, flash } of slot.unlocks) {
      expect(pip.style.backgroundColor ?? null).toBeNull()
      expect(pip.style.height ?? null).toBeNull()
      expect(cost.style.visibility ?? null).toBeNull()
      expect(flash.style.opacity ?? null).toBeNull()
    }
    expect(slot.icon.attributes.get(DRAWN_MARK) ?? '').toBe('')
  }
})

test('finds pips the game makes apart from the icons by where they sit', () => {
  // The four rows sit together, apart from the icons; each slot's row is
  // drawn and listened on as the one inside its icon would be.
  const hud = gameHud()
  const bar = barIcons(hud, true)
  const host = clock()
  const stock = new StockGuard(hud.game, host)
  stock.screen(
    parts(),
    abilityBarMods(screen({ id: '', abilityBarPresses: true })),
    displayed({
      player: 2,
      name: 'Ada',
      own: true,
      abilities: [{}, { ability: 12, upgrades: 0b11 }],
    }),
  )
  expect(bar[1]!.train.style.visibility).toBe('collapse')
  expect(bar[1]!.unlocks.map(({ pip }) => pip.style.backgroundColor)).toEqual([
    '#69e799',
    'black',
    'black',
  ])
  expect(bar[0]!.train.style.visibility ?? null).toBeNull()
  host.click(bar[3]!.pips)
  expect(host.pressed).toEqual(['arena ability:3'])
})

test('the interface layer mounts under the HUD tooltips it can reach', () => {
  const adopt = (parent: FakePanel, child: FakePanel) => {
    child.parent = parent
    parent.children.push(child)
    return child
  }
  const create = (type: string, parent: FakePanel, id: string) =>
    adopt(parent, new FakePanel(type, id))

  // The game's HUD holds its TooltipManager as a direct child, so the layer
  // goes just before it.
  const hud = new FakePanel('CitadelHud', 'Hud')
  const context = adopt(hud, new FakePanel('Panel', 'Context'))
  adopt(hud, new FakePanel('CitadelTooltipManager', 'TooltipManager'))
  adopt(hud, new FakePanel('Panel', 'LoadInFadeCover'))
  mountInterfaceLayer(create, context, 'ModlockUi')
  expect(hud.children.map((panel) => panel.id)).toEqual([
    'Context',
    'ModlockUi',
    'TooltipManager',
    'LoadInFadeCover',
  ])

  // A tree whose TooltipManager is not the top's child keeps the layer last.
  const top = new FakePanel('Panel', 'Top')
  const db = adopt(top, new FakePanel('CitadelDB', 'Dashboard'))
  adopt(db, new FakePanel('CitadelTooltipManager', 'TooltipManager'))
  mountInterfaceLayer(create, db, 'ModlockUi')
  expect(top.children.map((panel) => panel.id)).toEqual([
    'Dashboard',
    'ModlockUi',
  ])
})

/** menuScreen draws the same native controls across ordinary screen revisions. */
function menuScreen(mode: string, openRequest = 0, control = '41'): Screen {
  return screen(
    { id: '', children: ['surface'] },
    {
      id: 'surface',
      kind: 'KIND_MENU',
      menu: { mode, openRequest, openLabel: 'Settings' },
      children: ['close', 'go', 'form', 'output'],
    },
    { id: 'close', kind: 'KIND_MENU_CLOSE' },
    { id: 'go', kind: 'KIND_BUTTON' },
    { id: 'form', kind: 'KIND_DOCUMENT_INPUT', documentInput: { control } },
    {
      id: 'output',
      kind: 'KIND_DOCUMENT_OUTPUT',
      documentText: '雪🙂\n<b>Teaching text</b>',
    },
  )
}

test('game actions report each press once, as released or cancelled', () => {
  // Only the root's list counts, and the command names the enum numbers.
  const following = gameActionMods(
    Screen.fromJson({
      trees: [
        {
          mod: 'arena',
          nodes: [{ id: '', gameActions: ['GAME_ACTION_SCOREBOARD'] }],
        },
        {
          mod: 'brawl',
          nodes: [{ id: 'x', gameActions: ['GAME_ACTION_SCOREBOARD'] }],
        },
      ],
    }),
  )
  expect([...(following.get(GameAction.SCOREBOARD) ?? [])]).toEqual(['arena'])
  expect(
    gameActionCommand('arena', GameAction.SCOREBOARD, ActionPhase.CANCELLED),
  ).toBe('modlockgame arena 1 3')

  // A press reports pressed once and released once, ignoring repeats.
  const sent: string[] = []
  let stock: 'escape' | 'other' | null = null
  const actions = new GameActions(
    (mod, _, phase) => sent.push(`${mod}:${ActionPhase[phase]}`),
    { allowed: () => true, stock: () => stock },
    (_, callback) => scheduled.set(scheduled.size + 1000, callback),
  )
  actions.follow(following)
  actions.change(GameAction.SCOREBOARD, true)
  actions.change(GameAction.SCOREBOARD, true)
  actions.change(GameAction.SCOREBOARD, false)
  expect(sent).toEqual(['arena:PRESSED', 'arena:RELEASED'])

  // A stock overlay cancels a held press, and its release reports nothing.
  sent.length = 0
  actions.change(GameAction.SCOREBOARD, true)
  stock = 'other'
  runScheduled()
  actions.change(GameAction.SCOREBOARD, false)
  expect(sent).toEqual(['arena:PRESSED', 'arena:CANCELLED'])

  // A press made while the overlay holds input reports nothing at all, even
  // after the overlay closes before the release.
  sent.length = 0
  actions.change(GameAction.SCOREBOARD, true)
  stock = null
  actions.change(GameAction.SCOREBOARD, false)
  expect(sent).toEqual([])

  // A mod that stops following mid-press hears nothing more of it.
  actions.change(GameAction.SCOREBOARD, true)
  actions.follow(new Map())
  actions.change(GameAction.SCOREBOARD, false)
  expect(sent).toEqual(['arena:PRESSED'])
})

test('extra info reports a press while the game shows its detail view', () => {
  // The game shows Extra Info only as a class, which the action reads each
  // tick while a mod follows it.
  const sent: string[] = []
  const actions = new GameActions(
    (mod, action, phase) =>
      sent.push(`${mod}:${GameAction[action]}:${ActionPhase[phase]}`),
    { allowed: () => true, stock: () => null },
  )
  const ticks: (() => void)[] = []
  const tick = () => ticks.splice(0).forEach((callback) => callback())
  let detail = false
  let reads = 0
  const extraInfo = new ClassAction(
    GameAction.EXTRA_INFO,
    () => {
      reads++
      return detail
    },
    actions,
    (_, callback) => ticks.push(callback),
  )
  const following = new Map([[GameAction.EXTRA_INFO, new Set(['arena'])]])
  actions.follow(following)
  extraInfo.follow(following)
  detail = true
  tick()
  tick()
  detail = false
  tick()
  expect(sent).toEqual([
    'arena:EXTRA_INFO:PRESSED',
    'arena:EXTRA_INFO:RELEASED',
  ])

  // Following again does not start a second reader.
  extraInfo.follow(following)
  expect(ticks.length).toBe(1)

  // A press held as the mod stops following reports nothing more, and the
  // class is no longer read.
  sent.length = 0
  detail = true
  tick()
  actions.follow(new Map())
  extraInfo.follow(new Map())
  tick()
  const after = reads
  tick()
  expect(sent).toEqual(['arena:EXTRA_INFO:PRESSED'])
  expect(reads).toBe(after)
  expect(ticks.length).toBe(0)
})

test('overlay followers hear the current overlay, then each change', () => {
  // Only the root's flag counts, and the command names the enum number.
  expect([
    ...overlayMods(
      Screen.fromJson({
        trees: [
          { mod: 'arena', nodes: [{ id: '', followsOverlay: true }] },
          { mod: 'brawl', nodes: [{ id: 'x', followsOverlay: true }] },
        ],
      }),
    ),
  ]).toEqual(['arena'])
  expect(overlayCommand('arena', Overlay.ESCAPE_MENU)).toBe(
    'modlockoverlay arena 6',
  )

  // The reader names the first open overlay, and the menus see its kind.
  const hud = gameHud()
  const reader = new OverlayReader(hud.game)
  expect(reader.read()).toBe(Overlay.NONE)
  hud.game.AddClass('ShowEscapeMenu')
  hud.game.AddClass('gItemDraftOpen')
  expect(reader.read()).toBe(Overlay.ITEM_DRAFT)
  hud.chat.AddClass(CHAT_OPEN)
  expect(reader.read()).toBe(Overlay.CHAT)
  expect(stockOf(Overlay.CHAT)).toBe('other')
  expect(stockOf(Overlay.ESCAPE_MENU)).toBe('escape')
  expect(stockOf(Overlay.NONE)).toBeNull()

  // A chat box the game replaced is found again.
  hud.chat.DeleteAsync(0)
  expect(reader.read()).toBe(Overlay.ITEM_DRAFT)
  hud.add(hud.game, 'Chat', CHAT_OPEN)
  expect(reader.read()).toBe(Overlay.CHAT)

  // A new follower hears the current overlay at once; each change reaches
  // every follower once, and a repeat reading reports nothing.
  const sent: string[] = []
  let overlay = Overlay.NONE
  const follow = new OverlayFollow(
    () => overlay,
    (mod, now) => sent.push(`${mod}:${Overlay[now]}`),
    (_, callback) => scheduled.set(scheduled.size + 1000, callback),
  )
  follow.follow(new Set(['arena']))
  overlay = Overlay.SHOP
  runScheduled()
  runScheduled()
  follow.follow(new Set(['arena', 'brawl']))
  overlay = Overlay.NONE
  runScheduled()
  expect(sent).toEqual([
    'arena:NONE',
    'arena:SHOP',
    'brawl:SHOP',
    'arena:NONE',
    'brawl:NONE',
  ])

  // With no followers left the reading stops.
  follow.follow(new Set())
  overlay = Overlay.POPUP
  runScheduled()
  expect(scheduled.size).toBe(0)
  expect(sent).toHaveLength(5)
})

test('local menus close and return focus without replaying a press or open request', () => {
  // Use the actual drawing and its local state, under a controlled stock HUD.
  const root = new FakePanel('Panel', 'ModlockUi')
  const cursor: boolean[] = []
  const presses: string[] = []
  let stock: 'escape' | 'other' | null = null
  let allowed = true
  let resumed = 0
  const drawing = new Drawing(
    $,
    root,
    (_, node) => presses.push(node),
    undefined,
    undefined,
    {
      allowed: () => allowed,
      stock: () => stock,
      capture: (open) => cursor.push(open),
      resume: () => {
        resumed++
      },
    },
  )
  const state = menuScreen('MENU_MODE_HOLD')
  drawing.draw(state)
  const content = root.FindChildTraverse('ModlockMenuContent')!
  const close = content.children[0]!
  const go = content.children[1]!
  expect(content.visible).toBe(false)
  drawing.scoreboard(true)
  expect(content.visible).toBe(true)
  go.events.get('onactivate')!()
  expect(presses).toEqual(['go'])
  drawing.scoreboard(false)
  go.events.get('onactivate')!()
  expect(presses).toEqual(['go'])
  expect(content.visible).toBe(false)
  expect(dispatched.some(([event]) => event === 'DropInputFocus')).toBe(true)

  // A locally dismissed held menu does not reopen until a fresh press.
  drawing.scoreboard(true)
  close.events.get('onactivate')!()
  drawing.scoreboard(true)
  drawing.draw(state)
  expect(content.visible).toBe(false)
  drawing.scoreboard(false)
  drawing.scoreboard(true)
  const draft = root.FindChildTraverse('ModlockDocumentDraft')!
  draft.SetFocus()
  content.events.get('oncancel')!()
  expect(content.visible).toBe(false)
  drawing.scoreboard(false)

  // Stock chat/shop wins over a menu and a held key cannot revive it.
  drawing.scoreboard(true)
  stock = 'other'
  runScheduled()
  expect(content.visible).toBe(false)
  stock = null
  drawing.scoreboard(true)
  expect(content.visible).toBe(false)
  drawing.scoreboard(false)
  drawing.draw(menuScreen('MENU_MODE_TOGGLE', 1))
  expect(content.visible).toBe(true)
  drawing.scoreboard(true)
  drawing.scoreboard(false)
  drawing.draw(menuScreen('MENU_MODE_TOGGLE', 1))
  expect(content.visible).toBe(false)
  drawing.draw(menuScreen('MENU_MODE_TOGGLE', 2))
  stock = 'escape'
  runScheduled()
  expect(content.visible).toBe(false)
  expect(resumed).toBe(1)
  stock = null
  drawing.draw(menuScreen('MENU_MODE_TOGGLE', 3))
  allowed = false
  runScheduled()
  expect(content.visible).toBe(false)
  allowed = true
  const replacement = menuScreen('MENU_MODE_TOGGLE', 3)
  replacement.trees![0]!.generation = 2n
  drawing.draw(replacement)
  expect(root.FindChildTraverse('ModlockMenuContent')!.visible).toBe(false)
  expect(cursor[cursor.length - 1]).toBe(false)
})

test('document form sends only a whole explicit submit, declares an oversized draft, and abandons closed or replaced drafts', () => {
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  drawing.draw(menuScreen('MENU_MODE_TOGGLE', 1))
  const content = root.FindChildTraverse('ModlockMenuContent')!
  const form = root.FindChildTraverse('ModlockDocumentDraft')!
  const submit = root.FindChildTraverse('ModlockDocumentSubmit')!
  const output = content.children[3]!
  expect(output.text).toBe('雪🙂\n<b>Teaching text</b>')
  expect(output.properties).toMatchObject({
    allowtextselection: 'true',
    html: 'false',
  })
  form.text = '雪🙂\n'.repeat(6500)
  form.events.get('ontextentrychange')?.()
  form.events.get('oninputsubmit')?.()
  runScheduled()
  expect(dispatched).toEqual([])
  submit.events.get('onactivate')!()
  submit.events.get('onactivate')!()
  for (let i = 0; i < 100; i++) runScheduled()
  const lines = dispatched.map((dispatch) => dispatch[1]!)
  expect(lines[0]).toEndWith(':b:52000')
  expect(lines[lines.length - 1]).toEndWith(':e')
  const bytes = Buffer.concat(
    lines
      .slice(1, -1)
      .map((line) =>
        Buffer.from(line.slice(line.lastIndexOf(':') + 1), 'base64url'),
      ),
  )
  expect(bytes.toString()).toBe(form.text)

  dispatched.length = 0
  form.text = '雪'.repeat(88000)
  submit.events.get('onactivate')!()
  runScheduled()
  expect(dispatched).toHaveLength(1)
  const declared = Number(dispatched[0]![1]!.split(':b:')[1])
  expect(declared).toBeGreaterThan(262144)
  dispatched.length = 0
  form.text = 'Retry with a whole document.'
  submit.events.get('onactivate')!()
  drawing.scoreboard(true)
  runScheduled()
  expect(
    dispatched
      .filter(([event]) => event === 'CitadelConCommand')
      .map((dispatch) => dispatch[1]!.slice(dispatch[1]!.lastIndexOf(':') + 1)),
  ).toEqual(['28', 'x'])
  drawing.scoreboard(false)
  drawing.scoreboard(true)
  dispatched.length = 0
  submit.events.get('onactivate')!()
  drawing.draw(menuScreen('MENU_MODE_TOGGLE', 1, '42'))
  runScheduled()
  expect(
    dispatched
      .filter(([event]) => event === 'CitadelConCommand')
      .map((dispatch) => dispatch[1]!.slice(dispatch[1]!.lastIndexOf(':') + 1)),
  ).toEqual(['28', 'x'])
  expect(form.text).toBe('')
})

/** queueScreen draws arena's warning and alert menus and duel's one menu. */
function queueScreen(
  warn: number,
  alert: number,
  duel = 0,
  alertShown = true,
): Screen {
  const menu = (
    id: string,
    openRequest: number,
    priority: number,
  ): JsonObject[] => [
    {
      id,
      kind: 'KIND_MENU',
      menu: { openRequest, priority },
      children: [`${id}-name`],
    },
    { id: `${id}-name`, kind: 'KIND_LABEL', text: id },
  ]
  return Screen.fromJson({
    trees: [
      {
        mod: 'arena',
        nodes: [
          { id: '', children: alertShown ? ['warn', 'alert'] : ['warn'] },
          ...menu('warn', warn, 1),
          ...(alertShown ? menu('alert', alert, 2) : []),
        ],
      },
      {
        mod: 'duel',
        nodes: [{ id: '', children: ['other'] }, ...menu('other', duel, 0)],
      },
    ],
  })
}

/** menuContents finds the contents of the menu whose label reads name. */
function menuContents(root: FakePanel, name: string): FakePanel | undefined {
  for (const child of root.children) {
    if (child.id === 'ModlockMenuContent' && child.children[0]?.text === name)
      return child
    const found = menuContents(child, name)
    if (found) return found
  }
}

/** shownMenus names the menus whose contents are visible. */
function shownMenus(root: FakePanel): string[] {
  return ['warn', 'alert', 'other'].filter(
    (name) => menuContents(root, name)?.visible,
  )
}

test('a blocked open request waits, by priority within its mod, until nothing blocks it', () => {
  const root = new FakePanel('Panel', 'ModlockUi')
  let stock: 'escape' | 'other' | null = 'other'
  const drawing = new Drawing($, root, () => {}, undefined, undefined, {
    allowed: () => true,
    stock: () => stock,
    capture: () => {},
    resume: () => {},
  })

  // The shop declines the warning; it opens once the shop closes.
  drawing.draw(queueScreen(1, 0))
  expect(shownMenus(root)).toEqual([])
  runScheduled()
  expect(shownMenus(root)).toEqual([])
  stock = null
  runScheduled()
  expect(shownMenus(root)).toEqual(['warn'])

  // The alert outranks the warning; a newer warning waits behind it and
  // opens when the player dismisses the alert.
  drawing.draw(queueScreen(1, 1))
  expect(shownMenus(root)).toEqual(['alert'])
  drawing.draw(queueScreen(2, 1))
  expect(shownMenus(root)).toEqual(['alert'])
  menuContents(root, 'alert')!.events.get('oncancel')!()
  expect(shownMenus(root)).toEqual(['warn'])

  // A request whose menu the mod withdraws before the shop closes is gone.
  stock = 'other'
  runScheduled()
  drawing.draw(queueScreen(2, 2))
  drawing.draw(queueScreen(2, 2, 0, false))
  stock = null
  runScheduled()
  expect(shownMenus(root)).toEqual([])

  // Another mod's request never takes an open menu; it waits for dismissal.
  drawing.draw(queueScreen(3, 2))
  drawing.draw(queueScreen(3, 2, 1))
  expect(shownMenus(root)).toEqual(['warn'])
  menuContents(root, 'warn')!.events.get('oncancel')!()
  expect(shownMenus(root)).toEqual(['other'])
})

test('draws a root in the HUD frame and reads where each measured node sits', () => {
  // A root in the HUD frame hangs from one pass-through panel that takes the
  // game's own clamp class.
  const root = new FakePanel('Panel', 'ModlockUi')
  root.actuallayoutheight = 1080
  const drawing = new Drawing($, root, () => {})
  expect(drawing.placement().size).toBe(0)
  drawing.draw(
    screen(
      { id: '', children: ['a'], frame: 'FRAME_HUD', followsPlacement: true },
      { id: 'a', kind: 'KIND_PANEL', measure: 'preview' },
    ),
  )
  const hud = root.FindChild('ModlockHud')!
  expect(hud.classes).toContain('clamp_width')
  expect(hud.hittest).toBe(false)
  expect(hud.children).toHaveLength(1)

  // Boxes are in layout units from the screen's top left: each position and
  // size is window pixels, less the screen's origin, over the UI scale of 2.
  hud.actuallayoutheight = 1080
  const node = hud.children[0]!.children[0]!
  node.actuallayoutheight = 50
  node.GetPositionWithinWindow = () => ({ x: 140, y: 20 })
  const measured = drawing.placement().get('arena')!
  expect(measured.screen).toEqual([100, 1080])
  expect(measured.hud).toEqual([0, 0, 100, 1080])
  expect([...measured.boxes]).toEqual([['preview', [20, 20, 100, 50]]])

  // Moving the root back to the screen draws it anew under the screen.
  drawing.draw(screen({ id: '', children: [] }))
  expect(hud.children).toHaveLength(0)
  expect(root.children.filter((each) => each !== hud)).toHaveLength(1)
  expect(drawing.placement().size).toBe(0)
})

test('reports a placement whole, then each change, in lines that fit', () => {
  // A change too large for one line goes in several, each with the head.
  const head = 'modlockplacement arena 2520 1080 300 0 1920 1080'
  const many = Array.from(
    { length: 40 },
    (_, i): [string, [number, number, number, number]] => [
      `node${i}`,
      [i, i, 10, 10],
    ],
  )
  const lines = placementCommands(
    'arena',
    { screen: [2520, 1080], hud: [300, 0, 1920, 1080], boxes: new Map() },
    many,
  )
  expect(lines.length).toBeGreaterThan(1)
  for (const line of lines) {
    expect(line.startsWith(head + ' ')).toBe(true)
    expect(line.length).toBeLessThanOrEqual(maxPlacementLine)
  }
  expect(lines.join('').split(' node').length - 1).toBe(40)

  // A new follower hears everything; then only a changed box, or the head
  // alone when only the HUD moved, and nothing when nothing moved.
  const sent: string[] = []
  let hud: [number, number, number, number] = [300, 0, 1920, 1080]
  let bar: [number, number, number, number] = [10, 20, 30, 40]
  const read = () =>
    new Map([
      [
        'arena',
        {
          screen: [2520, 1080] as const,
          hud,
          boxes: new Map([
            ['preview', [1, 2, 3, 4] as const],
            ['bar', bar],
          ]),
        },
      ],
    ])
  const follow = new PlacementFollow(
    read,
    (line) => sent.push(line),
    (_, callback) => scheduled.set(scheduled.size + 1000, callback),
  )
  follow.follow(new Map([['arena', 1n]]))
  runScheduled()
  bar = [11, 20, 30, 40]
  runScheduled()
  hud = [0, 0, 2520, 1080]
  runScheduled()
  expect(sent).toEqual([
    `${head} preview 1 2 3 4 bar 10 20 30 40`,
    `${head} bar 11 20 30 40`,
    'modlockplacement arena 2520 1080 0 0 2520 1080',
  ])

  // A cleared tree, which the host forgets, hears everything again; once no
  // mod follows, the reading stops.
  sent.length = 0
  follow.follow(new Map([['arena', 2n]]))
  expect(sent).toEqual([
    'modlockplacement arena 2520 1080 0 0 2520 1080 preview 1 2 3 4 bar 11 20 30 40',
  ])
  follow.follow(new Map())
  runScheduled()
  expect(scheduled.size).toBe(0)
})

test('a form focuses its marked control on open and Enter presses its submit button', () => {
  const root = new FakePanel('Panel', 'ModlockUi')
  const sent: string[] = []
  const drawing = new Drawing(
    $,
    root,
    (mod, node) => sent.push(pressCommand(mod, node)),
    undefined,
    (mod, node, value) => sent.push(inputCommand(mod, node, value)),
  )
  const form = (openRequest: number) =>
    screen(
      { id: '', children: ['outside', 'surface'] },
      { id: 'outside', kind: 'KIND_BUTTON', submits: true },
      {
        id: 'surface',
        kind: 'KIND_MENU',
        menu: { openRequest },
        children: ['name', 'cancel', 'save'],
      },
      {
        id: 'name',
        kind: 'KIND_INPUT',
        focusOrder: 1,
        initialFocus: true,
        textEntry: {},
      },
      { id: 'cancel', kind: 'KIND_BUTTON', focusOrder: 3 },
      { id: 'save', kind: 'KIND_BUTTON', focusOrder: 2, submits: true },
    )
  drawing.draw(form(1))
  const content = root.FindChildTraverse('ModlockMenuContent')!
  const [name, cancel, save] = content.children

  // The navigation order is made into each panel, and the entry has the
  // keyboard as the menu opens.
  expect(name!.properties).toMatchObject({
    tabindex: '1',
    acceptsfocus: 'true',
  })
  expect(save!.properties).toMatchObject({ tabindex: '2' })
  expect(cancel!.properties).toMatchObject({ tabindex: '3' })
  expect(name!.focused).toBe(true)
  expect(content.focused).toBe(false)

  // Enter sends the typed name, then presses the menu's own submit button,
  // never the one outside the menu.
  name!.text = 'Anaïs'
  name!.events.get('ontextentrychange')!()
  name!.events.get('oninputsubmit')!()
  expect(sent).toEqual([
    `modlockinput arena name ${Buffer.from('Anaïs').toString('base64url')}`,
    'modlockpress arena save',
  ])
})

/**
 * listScreen draws a list of total rows, holding the rows from first, keyed
 * by keys or else by place, with extra list fields.
 */
function listScreen(
  total: number,
  first: number,
  count: number,
  extra: JsonObject = {},
  keys?: string[],
): Screen {
  const rows = Array.from(
    { length: Math.min(count, total - first) },
    (_, i) => ({
      key: keys?.[i] ?? `k${first + i}`,
      fields: {
        name: `row ${first + i}`,
        icon: `file://{images}/i${first + i}.png`,
      },
    }),
  )
  return screen(
    { id: '', children: ['scores'] },
    {
      id: 'scores',
      kind: 'KIND_LIST',
      style: { height: { pixels: 200 } },
      list: {
        rowHeight: 20,
        first,
        total,
        rows,
        row: [
          { id: '', children: ['name', 'icon'], style: { flow: 'FLOW_RIGHT' } },
          { id: 'name', kind: 'KIND_LABEL', text: '{name}!' },
          { id: 'icon', kind: 'KIND_IMAGE', image: '{icon}' },
        ],
        ...extra,
      },
    },
  )
}

/** descendants returns every panel under panel. */
function descendants(panel: FakePanel): FakePanel[] {
  return panel.children.flatMap((child) => [child, ...descendants(child)])
}

test('draws only the rows a list shows, however long the list', () => {
  const drawnFor = (total: number) => {
    // Draw the list with the window the host starts it with.
    const root = new FakePanel('Panel', 'ModlockUi')
    const windows: string[] = []
    const drawing = new Drawing(
      $,
      root,
      () => {},
      undefined,
      undefined,
      undefined,
      (mod, node, first, count) =>
        windows.push(`${mod} ${node} ${first} ${count}`),
    )
    drawing.draw(listScreen(total, 0, 50))
    const list = root.children[0]!.children[0]!
    const panels = descendants(root).length
    const texts = descendants(root)
      .filter((each) => each.paneltype === 'Label')
      .map((each) => each.text)
    const image = descendants(list).find((each) => each.paneltype === 'Image')
    const icon = image?.image || image?.properties?.src

    // Scroll near the end of the rows held, which asks for the next ones.
    for (let i = 0; i < 15; i++) handlers.get(list)!.get('ScrollDown')!()
    const blank = descendants(root).filter((each) => each.paneltype === 'Label')
    drawing.draw(listScreen(total, 32, 30))
    const scrolled = descendants(root)
      .filter((each) => each.paneltype === 'Label')
      .map((each) => each.text)
      .sort()
    return {
      panels,
      texts,
      icon,
      blank: blank.length,
      windows,
      scrolled,
      after: descendants(root).length,
      overflow: list.style.overflow,
    }
  }

  // A list of 10,000 rows builds the same panels as one of 100.
  const small = drawnFor(100)
  const large = drawnFor(10000)
  expect(large).toEqual(small)
  expect(small.texts).toEqual(Array.from({ length: 10 }, (_, i) => `row ${i}!`))
  expect(small.icon).toBe('file://{images}/i0.png')
  expect(small.overflow).toBe('clip clip')
  expect(small.windows).toEqual(['arena scores 26 30', 'arena scores 32 30'])
  expect(small.scrolled).toEqual(
    Array.from({ length: 10 }, (_, i) => `row ${45 + i}!`).sort(),
  )
  expect(small.after).toBe(small.panels)
})

test('measures a list in layout units at any UI scale', () => {
  // At 1024x768 the UI scale is 0.711, so a list 200 units tall lays out 142
  // window pixels tall and still shows all ten of its rows of 20.
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {})
  drawing.draw(listScreen(100, 0, 50))
  const list = root.children[0]!.children[0]!
  Object.assign(list, { actuallayoutheight: 142.2, actualuiscale_y: 0.711 })
  drawing.draw(listScreen(100, 0, 50))
  runScheduled()
  const texts = descendants(list)
    .filter((each) => each.paneltype === 'Label')
    .map((each) => each.text)
  expect(texts).toEqual(Array.from({ length: 10 }, (_, i) => `row ${i}!`))
})

test("keeps a list's focus on its row as rows come and go", () => {
  // Draw a list of 100 rows that paints its focused and selected rows.
  const root = new FakePanel('Panel', 'ModlockUi')
  const windows: string[] = []
  const chosen: string[] = []
  const drawing = new Drawing(
    $,
    root,
    () => {},
    undefined,
    (_mod, _node, value) => {
      if (value.case === 'row') chosen.push(value.value)
    },
    undefined,
    (_mod, _node, first, count, focus) =>
      windows.push(`${first} ${count} ${focus}`),
  )
  const looks = {
    focusStyle: { color: 0xff0000ff },
    selectedStyle: { bold: true },
  }
  const keys = (...each: string[]) => [
    ...each,
    ...Array.from({ length: 50 }, (_, i) => `k${i}`),
  ]
  drawing.draw(listScreen(100, 0, 50, looks))
  const list = root.children[0]!.children[0]!
  const slot = (place: number) =>
    descendants(list).find(
      (each) => each.paneltype === 'Label' && each.text === `row ${place}!`,
    )?.parent
  const sent = () => windows[windows.length - 1]!.split(' ')[2]

  // The first step focuses the first row shown, and each step after moves
  // it; the host hears each new focus with the window it holds.
  list.events.get('onmovedown')!()
  list.events.get('onmovedown')!()
  list.events.get('onmovedown')!()
  expect(windows[windows.length - 1]).toBe('0 50 k2')
  expect(slot(2)!.style.color).toBeTruthy()
  expect(slot(1)!.style.color).toBeFalsy()

  // A row ranked above the focus keeps the focus on its row, one place down,
  // so the next step focuses the row after it.
  drawing.draw(
    listScreen(101, 0, 50, { ...looks, focus: 'k2', focusAt: 3 }, keys('new')),
  )
  list.events.get('onmovedown')!()
  expect(sent()).toBe('k3')

  // When the focused row leaves, the focus takes the row now at its place;
  // an echo of an earlier focus moves nothing.
  const gone = keys('new').filter((key) => key !== 'k3')
  drawing.draw(listScreen(100, 0, 50, { ...looks, focus: 'k3' }, gone))
  expect(sent()).toBe('k4')
  drawing.draw(listScreen(100, 0, 50, { ...looks, focus: 'k3' }, gone))
  expect(sent()).toBe('k4')

  // Enter chooses the focused row, and a press chooses the pressed row and
  // focuses it; the mod draws the chosen row back as selected.
  list.events.get('onactivate')!()
  slot(0)!.events.get('onactivate')!()
  expect(chosen).toEqual(['k4', 'new'])
  expect(sent()).toBe('new')
  drawing.draw(
    listScreen(
      100,
      0,
      50,
      { ...looks, selected: 'new', focus: 'new', focusAt: 0 },
      gone,
    ),
  )
  expect(slot(0)!.style.fontWeight).toBeTruthy()
  expect(slot(1)!.style.fontWeight).toBeFalsy()

  // A list emptied clears the focus.
  drawing.draw(listScreen(0, 0, 0, { ...looks, focus: 'new' }))
  expect(windows[windows.length - 1]).toBe('0 1 ')
})
