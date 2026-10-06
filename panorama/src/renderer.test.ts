import type { JsonObject } from '@aptre/protobuf-es-lite'
import { beforeEach, expect, test } from 'bun:test'

import { Screen } from '../../proto/modlock/ui.pb.js'
import { Bridge } from './bridge.js'
import { Drawing } from './draw.js'

/** FakePanel records what the renderer does to one panel. */
class FakePanel {
  children: FakePanel[] = []
  parent: FakePanel | null = null
  hittest = true
  text = ''
  image = ''
  url = ''
  valid = true
  style: Record<string, string> = {}
  events = new Map<string, () => void>()

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
  FindChildTraverse() {
    return null
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

beforeEach(() => {
  handlers.clear()
  scheduled.clear()
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
    },
  })
})

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

  // A cycle draws each node once, and an empty screen removes the tree.
  drawing.draw(
    screen({ id: '', children: ['a'] }, { id: 'a', children: ['a', ''] }),
  )
  expect(tree.children.length).toBe(1)
  expect(tree.children[0]!.children).toEqual([])
  drawing.draw(null)
  expect(root.children).toEqual([])
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

  // A screen in two parts shows once both arrive, each acknowledged.
  const source = encodeURIComponent(
    JSON.stringify({ trees: [{ mod: 'arena', nodes: [{ id: '' }] }] }),
  )
  const half = Math.floor(source.length / 2)
  const part = (offset: number, chunk: string) =>
    `modlock:snapshot:${JSON.stringify({ id: 7, offset, total: source.length, chunk })}`

  // The ready title ends the page's load deadline.
  title('modlock:bridge-ready')
  expect(scheduled.size).toBe(0)

  // The first part is acknowledged but not shown.
  title(part(0, source.slice(0, half)))
  expect(JSON.parse(decodeURIComponent(state.url.split('#')[1]!))).toEqual({
    frameAck: 7,
    offset: half,
  })
  expect(shown).toEqual([])

  // The second part shows the screen.
  title(part(half, source.slice(half)))
  expect(shown.length).toBe(1)
  expect(shown[0]?.trees?.[0]?.mod).toBe('arena')

  // A part out of order is dropped, and a closed stream clears the screen.
  title(part(half, source.slice(half)).replace('"id":7', '"id":8'))
  expect(shown.length).toBe(1)
  title('')
  expect(shown).toEqual([shown[0]!, null])
})
