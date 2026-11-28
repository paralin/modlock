import { expect, test } from 'bun:test'

import { Kind, UiRequest } from '../../src/wasm/ui/ui.pb.js'
import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import { bind } from '../src/host.js'
import { jsx, show } from '../src/index.js'
import { Player } from '../src/host.gen.js'
import { change, left, windowed, type ListRow } from '../src/ui.js'

// The library sends each interface change through the transport, which
// records it. The list tests have a file of their own because each builds
// thousands of rows, and a scriptc suite file runs within one initialize.
const sent: UiRequest[] = []
bind((encoded) => {
  const call = Call.fromBinary(encoded)
  if (call.method === 'Ui') sent.push(UiRequest.fromBinary(call.request))
  return Reply.toBinary({})
})

/** listNode returns the list node of the last change, if it set one. */
function listNode() {
  return sent.at(-1)!.change!.set!.find((each) => each.kind === Kind.LIST)
}

test('a list sends the window of its rows in its node', () => {
  // The first draw sends the template and the first rows, in order of rank,
  // then sort, then key, the last of two rows with one key counting.
  const player = new Player(9)
  const board = (rows: { key: string; name: string; rank?: number }[]) =>
    jsx('list', {
      style: { height: 200 },
      rowHeight: 20,
      row: jsx('panel', {
        children: [
          jsx('label', { children: '{name}' }),
          jsx('image', { src: '{icon}' }),
        ],
      }),
      rows: rows.map(({ key, name, rank }) => ({
        key,
        rank,
        fields: { name },
      })),
    })
  show(
    player,
    board([
      { key: 'b', name: 'Bo', rank: 2 },
      { key: 'a', name: 'Ann', rank: 1 },
      { key: 'b', name: 'Bob', rank: 0 },
    ]),
  )
  const node = listNode()!
  expect(node.list!.rowHeight).toBe(20)
  expect(node.list!.row!.map((part) => part.id ?? '')).toEqual(['1', '2', ''])
  expect(node.list!.rows!.map((row) => row.fields!['name'])).toEqual([
    'Bob',
    'Ann',
  ])
  expect([node.list!.first ?? 0, node.list!.total]).toEqual([0, 2])

  // Changed rows send the list node again with its new window.
  show(
    player,
    board([
      { key: 'b', name: 'Bob' },
      { key: 'c', name: 'Cy' },
    ]),
  )
  expect(listNode()!.list!.rows!.map((row) => row.key)).toEqual(['b', 'c'])

  // The same rows in a new array send nothing.
  const count = sent.length
  show(
    player,
    board([
      { key: 'b', name: 'Bob' },
      { key: 'c', name: 'Cy' },
    ]),
  )
  expect(sent.length).toBe(count)
  left(player)
})

test('showing a list costs its window, however many rows it holds', () => {
  // A list of 1,000 rows and one of 10,000 send the same window, and showing
  // either again after a draw without it reads none of its rows: the last
  // row counts each read of its key, which any walk of the rows makes. The
  // rows are ListRow[], since scriptc copies an array of rows of another
  // shape each time it is passed as rows, and the copy is a new array.
  for (const length of [1000, 10000]) {
    const player = new Player(9)
    let walked = 0
    const held: ListRow[] = Array.from({ length }, (_, i) => ({
      key: `r${String(i).padStart(5, '0')}`,
      fields: { name: `Row ${i}` },
    }))
    held[length - 1] = {
      get key() {
        walked++
        return 'r99999'
      },
      fields: { name: 'last' },
    }
    const list = (shown: boolean) =>
      jsx('panel', {
        children: [
          jsx('label', { children: 'Scores' }),
          shown &&
            jsx('list', {
              rowHeight: 20,
              row: jsx('label', { children: '{name}' }),
              rows: held,
            }),
        ],
      })
    show(player, list(true))
    expect(listNode()!.list!.rows!.length).toBe(50)
    expect(listNode()!.list!.total).toBe(length)
    show(player, list(false))
    walked = 0
    show(player, list(true))
    expect(walked).toBe(0)
    expect(listNode()!.list!.rows!.length).toBe(50)
    left(player)
  }
})

test("a list draws the window and focus the player's screen asks for", () => {
  // A window event sends only the list node, with the rows it asks for and
  // the focused row's place in the list.
  const player = new Player(9)
  const rows = Array.from({ length: 300 }, (_, i) => ({
    key: `r${String(i).padStart(3, '0')}`,
  }))
  show(
    player,
    jsx('panel', {
      children: [
        jsx('label', { children: 'Scores' }),
        jsx('list', { rowHeight: 20, row: jsx('label', {}), rows }),
      ],
    }),
  )
  const id = listNode()!.id!
  windowed({ player, node: id, first: 100, count: 20, focus: 'r005' })
  const change = sent.at(-1)!.change!
  expect(change.set!.map((each) => each.id)).toEqual([id])
  const list = change.set![0]!.list!
  expect([list.first, list.rows!.length, list.rows![0]!.key]).toEqual([
    100,
    20,
    'r100',
  ])
  expect([list.focus, list.focusAt]).toEqual(['r005', 5])

  // A window past the end moves back to end at the last row, a focus the
  // list lacks has no place, and a window for no list sends nothing.
  windowed({ player, node: id, first: 290, count: 256, focus: 'gone' })
  const end = listNode()!.list!
  expect([end.first, end.rows!.length]).toEqual([44, 256])
  expect([end.focus, end.focusAt]).toEqual(['gone', undefined])
  const count = sent.length
  windowed({ player, node: 'missing', first: 0, count: 5, focus: '' })
  expect(sent.length).toBe(count)
  left(player)
})

test('a list hears the row a player chose and draws it as selected', () => {
  // A chosen row reaches onChange with its key, and the mod draws it back.
  const player = new Player(9)
  let selected = ''
  const board = () =>
    jsx('list', {
      rowHeight: 20,
      row: jsx('label', { children: '{name}' }),
      rows: [{ key: 'a', fields: { name: 'Ann' } }],
      selected,
      selectedStyle: { bold: true },
      onChange: (_: Player, key: string) => {
        selected = key
      },
    })
  show(player, board())
  const node = sent
    .at(-1)!
    .change!.set!.find((each) => each.kind === Kind.LIST)!
  expect(node.list!.selectedStyle?.bold).toBe(true)
  change(player, node.id!, 'a')
  expect(selected).toBe('a')
  show(player, board())
  const drawn = sent
    .at(-1)!
    .change!.set!.find((each) => each.kind === Kind.LIST)!
  expect(drawn.list!.selected).toBe('a')

  // A selected key the list does not hold is not drawn.
  selected = 'zed'
  show(player, board())
  expect(listNode()!.list!.selected).toBeUndefined()
})
