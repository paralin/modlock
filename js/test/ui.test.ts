import { expect, test } from 'bun:test'

import {
  Easing,
  Face,
  Flow,
  GradientShape,
  HeroLook,
  HudPart,
  Kind,
  Scaling,
  UiRequest,
  Weight,
} from '../../src/wasm/ui/ui.pb.js'
import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import { bind } from '../src/host.js'
import { Action, hideHud, jsx, show } from '../src/index.js'
import { change } from '../src/ui.js'

// The library sends each interface change through the transport, which
// records it.
const sent: UiRequest[] = []
bind((encoded) => {
  const call = Call.fromBinary(encoded)
  if (call.method === 'Ui') sent.push(UiRequest.fromBinary(call.request))
  return Reply.toBinary({})
})

/** keys is a panel of one label per key, the latest of them lit. */
function keys(lit: string) {
  return jsx('panel', {
    children: ['W', 'A', 'S', 'D'].map((key) =>
      jsx(
        'label',
        {
          style: { color: key === lit ? '#ffffff' : '#808080' },
          children: key,
        },
        key,
      ),
    ),
  })
}

test('a redraw sends only the nodes that changed, under their first ids', () => {
  // The first draw sends the whole screen.
  const player = { slot: 7 } as Parameters<typeof show>[0]
  show(player, keys('W'))
  const first = sent.at(-1)!.change!
  expect(first.reset).toBe(true)
  const ids = new Map(first.set!.map((node) => [node.text ?? '', node.id]))

  // Lighting another key changes its label and the one that went dark.
  show(player, keys('A'))
  const second = sent.at(-1)!.change!
  expect(second.reset ?? false).toBe(false)
  expect(second.set!.map((node) => node.text)).toEqual(['W', 'A'])
  expect(second.set!.map((node) => node.id)).toEqual([
    ids.get('W'),
    ids.get('A'),
  ])

  // The same screen again sends nothing.
  const count = sent.length
  show(player, keys('A'))
  expect(sent.length).toBe(count)

  // Hiding parts of the game's HUD changes only the root, which lists them,
  // and an empty list shows them again.
  hideHud(player, ['ownHealth', 'levelUp'])
  const hidden = sent.at(-1)!.change!
  expect(hidden.set!.map((node) => node.id ?? '')).toEqual([''])
  expect(hidden.set![0]!.hideHud).toEqual([
    HudPart.OWN_HEALTH,
    HudPart.LEVEL_UP,
  ])
  hideHud(player, [])
  expect(sent.at(-1)!.change!.set![0]!.hideHud ?? []).toEqual([])
})

test("a style names the game's faces, edges, paint and motion on the wire", () => {
  // Show a title in the heading face over a bordered plate.
  const player = { slot: 8 } as Parameters<typeof show>[0]
  show(
    player,
    jsx('panel', {
      style: { borderWidth: 2, borderColor: '#ffefd71a' },
      children: jsx('label', {
        style: {
          face: 'block',
          uppercase: true,
          letterSpacing: 1,
          shadow: true,
        },
        children: 'Round 4',
      }),
    }),
  )
  const nodes = sent.at(-1)!.change!.set!
  const plate = nodes.find((node) => node.style?.borderWidth)!
  const title = nodes.find((node) => node.text === 'Round 4')!

  // Each field arrives typed: the face as its enum, the color as RGBA.
  expect(plate.style!.borderColor).toBe(0xffefd71a)
  expect(title.style!.face).toBe(Face.BLOCK)
  expect(title.style!.uppercase).toBe(true)
  expect(title.style!.letterSpacing).toBe(1)
  expect(title.style!.shadow).toBe(true)

  // A row of a hero card and a disabled action wraps over a gradient.
  show(
    player,
    jsx('panel', {
      style: {
        flow: 'rightWrap',
        gradient: { shape: 'right', colors: ['#000000', '#ffffff'] },
        weight: 'semiBold',
      },
      children: [
        jsx('hero', { hero: 15, look: 'card', scaling: 'cover' }),
        Action({ kind: 'primary', children: 'Ready' }),
        Action({ disabled: true, children: 'Leave' }),
      ],
    }),
  )

  // The row's flow, weight and gradient arrive as their enums and stops.
  const drawn = sent.at(-1)!.change!.set!
  const row = drawn.find((node) => node.style?.gradient)!
  expect(row.style!.flow).toBe(Flow.RIGHT_WRAP)
  expect(row.style!.weight).toBe(Weight.SEMI_BOLD)
  expect(row.style!.gradient).toEqual({
    shape: GradientShape.RIGHT,
    stops: [{ color: 0x000000ff }, { at: 1, color: 0xffffffff }],
  })

  // The hero card names its hero, look and scaling.
  const hero = drawn.find((node) => node.kind === Kind.HERO_IMAGE)!
  expect(hero.hero).toBe(15)
  expect(hero.heroLook).toBe(HeroLook.CARD)
  expect(hero.scaling).toBe(Scaling.COVER)

  // An action carries its hover style, sounds and easing; a disabled one
  // answers neither the pointer nor a press.
  const [action, leave] = drawn.filter((node) => node.kind === Kind.BUTTON)
  expect(action!.hover).toBeDefined()
  expect(action!.sound).toBe('UI.MainMenu.Activate')
  expect(action!.hoverSound).toBe('UI.MainMenu.Hover')
  expect(action!.style!.transition!.easing ?? Easing.EASE_OUT).toBe(
    Easing.EASE_OUT,
  )
  expect(leave!.disabled).toBe(true)
  expect(leave!.hover).toBeUndefined()
})

test('inputs carry their values and hand changes to the mod', () => {
  // Show a name entry, a slider, a disabled toggle, an avatar and a token.
  const player = { slot: 9 } as Parameters<typeof show>[0]
  const names: string[] = []
  show(
    player,
    jsx('panel', {
      children: [
        jsx('entry', {
          value: 'Ana',
          placeholder: 'Name',
          maxLength: 12,
          onChange: (_: unknown, value: string) => names.push(value),
        }),
        jsx('slider', { value: 5, min: 0, max: 10, step: 1 }),
        jsx('toggle', {
          on: true,
          disabled: true,
          onChange: () => names.push('toggled'),
        }),
        jsx('avatar', { steamId: 76561198000000001n }),
        jsx('label', {
          token: '#Citadel_HeroName_Haze',
          variables: { count: 3 },
        }),
      ],
    }),
  )
  const nodes = sent.at(-1)!.change!.set!
  const [entry, slider, toggle] = nodes.filter(
    (node) => node.kind === Kind.INPUT,
  )

  // Each control arrives with its value and bounds.
  expect(entry!.input).toEqual({
    case: 'textEntry',
    value: { text: 'Ana', placeholder: 'Name', maxLength: 12 },
  })
  expect(slider!.input).toMatchObject({
    case: 'slider',
    value: { value: 5, max: 10, step: 1 },
  })
  expect(toggle!.disabled).toBe(true)
  expect(nodes.find((node) => node.kind === Kind.AVATAR)!.steamId).toBe(
    76561198000000001n,
  )
  const label = nodes.find((node) => node.token)!
  expect(label.variables).toEqual({ count: '3' })

  // A change reaches the entry's handler, and none the disabled toggle's.
  change(player, entry!.id!, 'Bo')
  change(player, toggle!.id!, false)
  expect(names).toEqual(['Bo'])
})
