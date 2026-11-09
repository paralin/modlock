import type { JsonObject } from '@aptre/protobuf-es-lite'
import { beforeEach, expect, test } from 'bun:test'

import { HudPart, Screen } from '../../src/wasm/ui/ui.pb.js'
import { Bridge } from './bridge.js'
import {
  AddonLayouts,
  Drawing,
  type InputValue,
  inputCommand,
  layoutDataChanged,
} from './draw.js'
import {
  ALL_MARK,
  CONNECTED_TO_HIDEOUT,
  HIDDEN_MARK,
  HudParts,
  LEASE_SECONDS,
  StockGuard,
  WALK_SECONDS,
} from './guard.js'
import { Cursor, cursorAsks, cursorFreed, hiddenParts } from './hud.js'
import { decodeUtf8 } from './packed.js'

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
  GetPositionWithinWindow() {
    return { x: 100, y: 0 }
  }
  checked = false
  visible = true
  image = ''
  url = ''
  layout = ''
  valid = true
  style: Record<string, string | null> = {}
  events = new Map<string, () => void>()
  attributes = new Map<string, string>()
  classes: string[] = []
  classChanges: string[] = []
  dialog: Record<string, string> = {}
  options: FakePanel[] = []
  selected = ''
  deleteSeconds = -1

  constructor(
    readonly paneltype: string,
    readonly id: string,
    readonly properties?: Record<string, string>,
  ) {}

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
  SetAttributeString(name: string, value: string) {
    this.attributes.set(name, value)
  }

  GetAttributeString(name: string, fallback: string) {
    return this.attributes.get(name) ?? fallback
  }
  BLoadLayout(path: string) {
    this.layout = path
    return !path.includes('missing')
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

beforeEach(() => {
  // Forget the last test's calls and install a fake $.
  handlers.clear()
  scheduled.clear()
  dispatched.length = 0
  messages.length = 0
  let handles = 0
  Object.assign(globalThis, {
    $: {
      Msg(...values: unknown[]) {
        messages.push(values.join(' '))
      },
      MousePosition() {
        return { x: 200, y: 0 }
      },
      CreatePanel(
        type: string,
        parent: FakePanel,
        id: string,
        properties?: Record<string, string>,
      ) {
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

test('draws inputs, avatars and game text, and sends edits', () => {
  // Draw an entry, a slider, a toggle, a dropdown, an avatar and a token.
  const root = new FakePanel('Panel', 'ModlockUi')
  const changes: string[] = []
  const drawing = new Drawing(
    $,
    root,
    () => {},
    undefined,
    (mod, node, value) => changes.push(inputCommand(mod, node, value)),
  )
  const tree = (name: string, disabled = false) =>
    screen(
      { id: '', children: ['name', 'volume', 'ready', 'team', 'face', 'hero'] },
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
    )
  drawing.draw(tree('Ana'))
  const [name, volume, ready, team, face, hero] = root.children[0]!.children

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

  // Focus brightens the border until the entry lets the keyboard go.
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
    'modlockinput arena ready 1',
    'modlockinput arena team 0',
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

  // A wheel event carries the scaled pointer position, without a seek.
  const before = panel.value
  expect(handlers.get(panel)!.get('ScrollUp')!()).toBe(true)
  expect(changes[changes.length - 1]).toEqual({
    case: 'wheel',
    value: { delta: 1, fraction: 0.25 },
  })
  expect(panel.value).toBe(before)
  expect(inputCommand('arena', 'time', changes[changes.length - 1]!)).toBe(
    'modlockinput arena time wheel:1:0.25',
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

  // Lay out the souls, the minimap, the abilities and the stats.
  const souls = add(game, 'gold_and_ap_container')
  const minimap = add(game, 'minimap_persp')
  const abilities = add(game, 'AbilitiesContainer')
  const stats = add(game, 'hudPlayerStats')
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
  }
  return host
}

/** parts reads the parts a screen whose one root hides names claims. */
function parts(...names: string[]) {
  return hiddenParts(screen({ id: '', hideHud: names }))
}

test("hides the game's HUD parts while a live screen claims them", () => {
  const hud = gameHud()
  const host = clock()
  const stock = new StockGuard(hud.game, host)

  // Hiding the player's health hides its panels, marked as shown, and keeps
  // the rest.
  stock.screen(parts('HUD_PART_OWN_HEALTH'))
  expect(hud.bars.visible).toBe(false)
  expect(hud.bars.attributes.get(HIDDEN_MARK)).toBe('shown')
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

  // The souls, minimap, abilities and stats hide whole.
  stock.screen(
    parts(
      'HUD_PART_SOULS',
      'HUD_PART_MINIMAP',
      'HUD_PART_ABILITIES',
      'HUD_PART_STATS',
    ),
  )
  for (const panel of [hud.souls, hud.minimap, hud.abilities, hud.stats])
    expect(panel.visible).toBe(false)
  expect(hud.health.visible).toBe(true)

  // A relay that ended shows everything again, and the ticks stop.
  stock.screen(null)
  for (const panel of [hud.souls, hud.boon, later, hud.clip])
    expect(panel.visible).toBe(true)
  runScheduled()
  expect(scheduled.size).toBe(0)
})

test('shows again a panel the guard hid only if it showed before', () => {
  // A panel the game keeps hidden, as gameplay_hud is until a team is
  // joined, is marked unseen and left hidden on restore.
  const hud = gameHud()
  hud.minimap.visible = false
  const stock = new StockGuard(hud.game, clock())
  stock.screen(parts('HUD_PART_MINIMAP', 'HUD_PART_SOULS'))
  expect(hud.minimap.attributes.get(HIDDEN_MARK)).toBe('unseen')
  stock.screen([])
  expect(hud.minimap.visible).toBe(false)
  expect(hud.minimap.attributes.get(HIDDEN_MARK)).toBe('')
  expect(hud.souls.visible).toBe(true)
})

test('hides again a panel the game showed again', () => {
  // The game showing a hidden panel again is undone on the next tick.
  const hud = gameHud()
  const stock = new StockGuard(hud.game, clock())
  stock.screen(parts('HUD_PART_SOULS'))
  hud.souls.visible = true
  runScheduled()
  expect(hud.souls.visible).toBe(false)
  expect(hud.souls.attributes.get(HIDDEN_MARK)).toBe('shown')
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
  // A reloaded script's guard restores every mark its predecessor left.
  const hud = gameHud()
  new StockGuard(hud.game, clock()).screen(
    parts('HUD_PART_SOULS', 'HUD_PART_TOP_BAR'),
  )
  scheduled.clear()
  hud.clip.style.visibility = 'collapse'
  new StockGuard(hud.game, clock())
  expect(hud.souls.visible).toBe(true)
  expect(hud.scores.visible).toBe(true)
  expect(hud.souls.attributes.get(HIDDEN_MARK)).toBe('')
  expect(hud.clip.style.visibility).toBeNull()
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
  // one of the game's panels, Hyperline's panel and the game's tooltips.
  const hud = gameHud()
  const host = clock()
  const layouts = hud.add(hud.game, 'HudLayouts')
  const layout = hud.add(layouts, '')
  const inner = hud.add(layout, '')
  const beside = hud.add(layouts, 'GameLayout')
  const stays = [
    hud.add(hud.game, 'ModlockUi'),
    hud.add(hud.game, 'HyperlineResident'),
    hud.add(hud.game, 'TooltipManager'),
    layout,
  ]
  const stock = new StockGuard(hud.game, host, [layout])

  // Everything else hides, and the way down to the layout shows.
  stock.screen(parts('HUD_PART_ALL'))
  for (const panel of [hud.health, hud.announcements, hud.souls, beside])
    expect(panel.visible).toBe(false)
  for (const panel of [...stays, layouts, inner])
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

test("hands a mod's routes to each of its layouts", () => {
  // Draw one layout, with routes built the way the controller sends them.
  const root = new FakePanel('Panel', 'ModlockUi')
  const drawing = new Drawing($, root, () => {}, {
    url: (mod, path) => `file://${mod}/${path}`,
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
