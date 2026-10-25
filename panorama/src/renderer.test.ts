import type { JsonObject } from '@aptre/protobuf-es-lite'
import { beforeEach, expect, test } from 'bun:test'

import { Screen } from '../../src/wasm/ui/ui.pb.js'
import { Bridge } from './bridge.js'
import { Drawing, layoutDataChanged } from './draw.js'
import { Hud } from './hud.js'
import { decodeUtf8 } from './packed.js'

/** FakePanel records what the renderer does to one panel. */
class FakePanel {
  children: FakePanel[] = []
  parent: FakePanel | null = null
  hittest = true
  text = ''
  image = ''
  url = ''
  layout = ''
  valid = true
  style: Record<string, string | null> = {}
  events = new Map<string, () => void>()
  attributes = new Map<string, string>()
  classes: string[] = []

  constructor(
    readonly paneltype: string,
    readonly id: string,
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
  descendants(): FakePanel[] {
    return this.children.flatMap((child) => [child, ...child.descendants()])
  }
  DeleteAsync() {
    this.valid = false
    this.parent?.children.splice(this.parent.children.indexOf(this), 1)
  }
}

/** handlers holds each panel's event handlers, as $.RegisterEventHandler keeps them. */
const handlers = new Map<
  FakePanel,
  Map<string, (...args: unknown[]) => unknown>
>()

/** scheduled holds the callbacks $.Schedule holds. */
const scheduled = new Map<number, () => void>()

/** dispatched holds each event $.DispatchEvent sent, with its arguments. */
const dispatched: string[][] = []

beforeEach(() => {
  // Forget the last test's calls and install a fake $.
  handlers.clear()
  scheduled.clear()
  dispatched.length = 0
  let handles = 0
  Object.assign(globalThis, {
    $: {
      Msg() {},
      CreatePanel(type: string, parent: FakePanel, id: string) {
        const panel = new FakePanel(type, id)
        panel.parent = parent
        parent.children.push(panel)
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

  // A cycle draws each node once, and an empty screen removes the tree.
  drawing.draw(
    screen({ id: '', children: ['a'] }, { id: 'a', children: ['a', ''] }),
  )
  expect(tree.children.length).toBe(1)
  expect(tree.children[0]!.children).toEqual([])
  drawing.draw(null)
  expect(root.children).toEqual([])
})

test("draws the game's faces, capitals, shadows and borders", () => {
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
})

test("hides the game's HUD parts while a root lists them", () => {
  // Build the game's HUD: the health beside the abilities, and the
  // announcements that hold a level-up notice.
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

  // Sweep through the fake $.Schedule.
  const hud = new Hud(game, (_, callback) => $.Schedule(0, callback))

  // Hiding the player's health collapses its panels and keeps the rest.
  hud.draw(screen({ id: '', hideHud: ['HUD_PART_OWN_HEALTH'] }))
  expect(bars.style.visibility).toBe('collapse')
  expect(shields.style.visibility).toBe('collapse')
  expect(held.style.visibility).toBeUndefined()
  expect(boon.style.visibility).toBeUndefined()

  // A level-up notice the game shows later collapses on the next sweep.
  hud.draw(
    screen({ id: '', hideHud: ['HUD_PART_OWN_HEALTH', 'HUD_PART_LEVEL_UP'] }),
  )
  expect(boon.style.visibility).toBe('collapse')
  expect(scores.style.visibility).toBeUndefined()
  const later = add(announcements, '', 'LevelUpBlock', 'PowerIncreaseBlock')
  expect(scheduled.size).toBe(1)
  runScheduled()
  expect(later.style.visibility).toBe('collapse')

  // The top bar collapses all but its chat.
  hud.draw(
    screen({
      id: '',
      hideHud: ['HUD_PART_OWN_HEALTH', 'HUD_PART_LEVEL_UP', 'HUD_PART_TOP_BAR'],
    }),
  )
  expect(scores.style.visibility).toBe('collapse')
  expect(clip.style.visibility).toBeUndefined()

  // Ammo collapses only its own panels, not the crosshair.
  hud.draw(
    screen({
      id: '',
      hideHud: ['HUD_PART_LEVEL_UP', 'HUD_PART_AMMO'],
    }),
  )
  expect(clip.style.visibility).toBe('collapse')
  expect(lowAmmo.style.visibility).toBe('collapse')
  expect(crosshair.style.visibility).toBeUndefined()
  expect(scores.style.visibility).toBeNull()
  expect(chat.style.visibility).toBeUndefined()

  // Once no root lists them, the parts show again and the sweeps stop.
  hud.draw(screen({ id: '', hideHud: ['HUD_PART_LEVEL_UP'] }))
  expect(bars.style.visibility).toBeNull()
  expect(scores.style.visibility).toBeNull()
  expect(later.style.visibility).toBe('collapse')
  hud.draw(null)
  expect(later.style.visibility).toBeNull()
  runScheduled()
  expect(scheduled.size).toBe(0)
})

test('loads layouts a host allows and hands them data', () => {
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

  // A layout that failed to load tries again on the next draw.
  drawing.draw(layout('missing.xml', ''))
  expect(panel.valid).toBe(false)
  expect(unloaded).toBe(1)
  const missing = root.children[0]!.children[0]!
  drawing.draw(layout('missing.xml', ''))
  expect(root.children[0]!.children[0]).toBe(missing)
  expect(missing.layout).toBe('file://arena/missing.xml')

  // A path leaving the addon and a host that refuses the mod both draw an
  // empty panel.
  drawing.draw(layout('../session.xml', ''))
  expect(root.children[0]!.children[0]!.layout).toBe('')
  new Drawing($, root, () => {}).draw(layout('shop.xml', ''))
  expect(root.children[root.children.length - 1]!.children[0]!.layout).toBe('')

  // Removing the tree deletes the layout's panel and lets the host rebind.
  drawing.draw(layout('shop.xml', ''))
  unloaded = 0
  drawing.draw(null)
  expect(unloaded).toBe(1)
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
