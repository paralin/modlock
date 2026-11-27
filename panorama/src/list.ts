// A list node scrolls a keyed run of rows far longer than a tree may hold.
// The host sends the renderer only a window of the rows, and the renderer
// builds panels only for the rows on screen: each list becomes a clipped
// panel holding one tall content panel, which holds a fixed pool of row
// panels drawn from the list's row template. Scrolling moves the content
// panel and refills the pool, so the panels drawn stay bounded by the
// viewport, whatever the list's length.
//
// The list panel holds the keyboard focus as one panel, and the renderer
// keeps which row it focuses by the row's key, so the focus stays on that row
// as rows arrive and leave around it. Each window request names the focused
// key, and the host echoes it with the row's place, which keeps the focus on
// a row scrolled past the window and tells the renderer when the row has
// left. The row then chosen is the one at the same place. Choosing a row
// tells the mod, which owns the selection and draws it back.

import {
  Kind,
  type Length,
  type List,
  type Node,
  Overflow,
  type Row,
  type Style,
} from '../../src/wasm/ui/ui.pb.js'

/** MAX_WINDOW is the most rows one window request asks the host for. */
export const MAX_WINDOW = 256

/** SCROLL_ROWS is the number of rows one wheel step moves a list. */
export const SCROLL_ROWS = 3

/** FALLBACK_ROWS is the viewport in rows of a list not yet laid out. */
const FALLBACK_ROWS = 10

/**
 * Window asks the host for the run of a list's rows starting at first,
 * count long, and tells it the key of the focused row, or "" for none.
 */
export type Window = (
  mod: string,
  node: string,
  first: number,
  count: number,
  focus: string,
) => void

/**
 * Focus is the row a player focuses in one list: none, a place whose row the
 * renderer does not hold yet, or a held row's key and place.
 */
export interface Focus {
  /** key is the focused row's key, or "" while no held row is focused. */
  key: string
  /** at is the focused row's place, or -1 for none. */
  at: number
}

/** Scroll is where one list is scrolled to and how much of it shows. */
export interface Scroll {
  /** offset is the distance scrolled from the first row, in pixels. */
  offset: number
  /** viewport is the height the list shows, in pixels; 0 until laid out. */
  viewport: number
  /**
   * asked is the window last asked for, which the host answers with the
   * next screen; a scroll it still covers asks for nothing more.
   */
  asked: Span
  /** panel is the panel the wheel handlers are bound to. */
  panel: unknown
  /** waited is true once a draw waited for the list to be laid out. */
  waited: boolean
  /** node is the list node last drawn, which the wheel scrolls. */
  node: Node
  /** focus is the row the player focuses. */
  focus: Focus
  /** sent is the focused key the host was last told. */
  sent: string
  /** slots maps each drawn row panel's id to the key of the row it shows. */
  slots: Map<string, string>
}

/** pixels returns a length of n pixels. */
function pixels(n: number): Length {
  return { value: { case: 'pixels', value: n } }
}

/** rowHeight returns a list's row height, at least one pixel. */
function rowHeight(list: List): number {
  const height = list.rowHeight ?? 0
  return Number.isFinite(height) && height >= 1 ? height : 1
}

/**
 * viewportOf returns the height a list shows: its measured height once laid
 * out, else its own pixel height, else FALLBACK_ROWS rows.
 */
function viewportOf(node: Node, scroll: Scroll, height: number): number {
  if (scroll.viewport > 0) return scroll.viewport
  const own = node.style?.height?.value
  if (own?.case === 'pixels' && own.value > 0) return own.value
  return FALLBACK_ROWS * height
}

/** heldKey returns the key of the row at place at, or "" if not held. */
function heldKey(list: List, at: number): string {
  const first = list.first ?? 0
  return list.rows?.[at - first]?.key ?? ''
}

/** fill replaces each "{field}" in text with the row's field, or nothing. */
function fill(text: string, row: Row): string {
  return text.replace(
    /\{(\w+)\}/g,
    (_, field: string) => row.fields?.[field] ?? '',
  )
}

/** placeholder returns the field an image names as "{field}", if it does. */
function placeholder(image: string): string | null {
  return /^\{(\w+)\}$/.exec(image)?.[1] ?? null
}

/**
 * Span is a run of row places: from first, up to but not including end.
 */
export interface Span {
  first: number
  end: number
}

/** visible returns the rows a list at scroll shows. */
export function visible(
  total: number,
  height: number,
  offset: number,
  viewport: number,
): Span {
  const first = Math.min(total, Math.max(0, Math.floor(offset / height)))
  const end = Math.min(total, Math.ceil((offset + viewport) / height))
  return { first, end: Math.max(first, end) }
}

/** covers reports whether outer holds every row of inner. */
function covers(outer: Span, inner: Span): boolean {
  return outer.first <= inner.first && inner.end <= outer.end
}

/**
 * wanted returns the window to ask for once neither the rows held nor the
 * window last asked for covers the rows shown with half a screen on either
 * side: the rows shown with a whole screen on either side, at most
 * MAX_WINDOW. It returns null while they cover that margin, so the next rows
 * arrive before they scroll in, and a scroll waiting on the host asks once.
 */
export function wanted(
  total: number,
  held: Span,
  asked: Span,
  shown: Span,
): Span | null {
  // Ask for nothing while the rows held or asked for cover the margin.
  const span = Math.max(1, shown.end - shown.first)
  const margin = Math.ceil(span / 2)
  const need = {
    first: Math.max(0, shown.first - margin),
    end: Math.min(total, shown.end + margin),
  }
  if (covers(held, need) || covers(asked, need)) return null

  // Ask for a screen on either side, at most MAX_WINDOW.
  const first = Math.max(0, shown.first - span)
  const end = Math.min(total, shown.end + span, first + MAX_WINDOW)
  return { first, end }
}

/**
 * Lists keeps each list's scroll by mod and node, and expands list nodes
 * into the panels the drawing builds.
 */
export class Lists {
  /** scrolls holds each drawn list's scroll, by mod, then node id. */
  private readonly scrolls = new Map<string, Map<string, Scroll>>()

  constructor(private readonly window?: Window) {}

  /** scroll returns the scroll of a list, making it the first time. */
  scroll(mod: string, id: string): Scroll {
    // Find the mod's lists, then the list's scroll.
    let lists = this.scrolls.get(mod)
    if (!lists) this.scrolls.set(mod, (lists = new Map<string, Scroll>()))
    let scroll = lists.get(id)
    if (!scroll) {
      scroll = {
        offset: 0,
        viewport: 0,
        asked: { first: 0, end: 0 },
        panel: null,
        waited: false,
        node: { id },
        focus: { key: '', at: -1 },
        sent: '',
        slots: new Map(),
      }
      lists.set(id, scroll)
    }
    return scroll
  }

  /** drop forgets the scroll of every list of mod. */
  drop(mod: string): void {
    this.scrolls.delete(mod)
  }

  /**
   * expand replaces each list node in nodes with the panels that draw its
   * shown rows, asks the host for the rows the screen needs next, and forgets
   * the scroll of each list mod no longer draws. It returns the ids of the
   * lists it expanded.
   */
  expand(mod: string, nodes: Map<string, Node>): string[] {
    // Find the lists; a mod with none keeps its nodes as they are.
    const ids = [...nodes.values()]
      .filter((node) => node.kind === Kind.LIST && node.list)
      .map((node) => node.id ?? '')
    const lists = this.scrolls.get(mod)
    for (const id of lists?.keys() ?? []) {
      if (!ids.includes(id)) lists!.delete(id)
    }
    for (const id of ids) {
      this.expandOne(mod, nodes.get(id)!, nodes)
    }
    return ids
  }

  /**
   * step scrolls a list by SCROLL_ROWS rows per step, down for a positive
   * step, kept within the list. It reports whether the scroll moved.
   */
  step(mod: string, id: string, steps: number): boolean {
    // The scroll runs from the first row to the last row's bottom at the
    // viewport's bottom.
    const scroll = this.scroll(mod, id)
    const node = scroll.node
    const list = node.list ?? {}
    const height = rowHeight(list)
    const viewport = viewportOf(node, scroll, height)
    const end = Math.max(0, (list.total ?? 0) * height - viewport)

    // Move it by the steps, kept within that run.
    const offset = Math.min(
      end,
      Math.max(0, scroll.offset + steps * SCROLL_ROWS * height),
    )
    if (offset === scroll.offset) return false
    scroll.offset = offset
    return true
  }

  /**
   * move steps a list's focus by delta rows, kept within the list, and
   * scrolls it into view; with no row focused, it focuses the first row
   * shown. It reports whether the focus moved.
   */
  move(mod: string, id: string, delta: number): boolean {
    // Find the place to focus.
    const scroll = this.scroll(mod, id)
    const list = scroll.node.list ?? {}
    const total = list.total ?? 0
    const height = rowHeight(list)
    if (total === 0) return false
    const at =
      scroll.focus.at < 0
        ? Math.min(total - 1, Math.floor(scroll.offset / height))
        : Math.min(total - 1, Math.max(0, scroll.focus.at + delta))
    if (at === scroll.focus.at) return false

    // Focus it, and scroll by as little as shows it whole.
    scroll.focus = { key: heldKey(list, at), at }
    const viewport = viewportOf(scroll.node, scroll, height)
    scroll.offset = Math.min(
      at * height,
      Math.max(scroll.offset, (at + 1) * height - viewport),
    )
    return true
  }

  /**
   * focusSlot focuses the row a list's row panel shows, and returns its key,
   * or "" when the panel shows none.
   */
  focusSlot(mod: string, id: string, slot: string): string {
    // Find the row the slot shows among the window's rows, and focus it.
    const scroll = this.scroll(mod, id)
    const key = scroll.slots.get(slot) ?? ''
    const list = scroll.node.list ?? {}
    const at = (list.rows ?? []).findIndex((row) => row.key === key)
    if (key === '' || at < 0) return ''
    scroll.focus = { key, at: (list.first ?? 0) + at }
    return key
  }

  /**
   * follow keeps a list's focus on its row as the host's rows change: at the
   * row's place while the rows held or the host's echo place it, and, once
   * the echo says the row has left, on the row that took its place. An echo
   * of an earlier focus says nothing about this one.
   */
  private follow(scroll: Scroll, list: List): void {
    // Find the focused row among the rows held, else in the echo.
    const focus = scroll.focus
    const first = list.first ?? 0
    const held = (list.rows ?? []).findIndex((row) => row.key === focus.key)
    if (focus.key !== '' && held >= 0) {
      focus.at = first + held
    } else if (focus.key !== '' && list.focus === focus.key) {
      if (list.focusAt === undefined) focus.key = ''
      else focus.at = list.focusAt
    }

    // Keep the place within the list, and take the key of the row there.
    const total = list.total ?? 0
    focus.at = Math.min(focus.at, total - 1)
    if (focus.key === '' && focus.at >= 0) focus.key = heldKey(list, focus.at)
  }

  /** expandOne replaces one list node with its content and row panels. */
  private expandOne(mod: string, node: Node, nodes: Map<string, Node>): void {
    // Keep the scroll inside the list's rows, as rows come and go.
    const id = node.id ?? ''
    const list = node.list!
    const height = rowHeight(list)
    const total = list.total ?? 0
    const scroll = this.scroll(mod, id)
    scroll.node = node
    const viewport = viewportOf(node, scroll, height)
    scroll.offset = Math.min(
      scroll.offset,
      Math.max(0, total * height - viewport),
    )

    // Keep the focus on its row.
    this.follow(scroll, list)

    // The pool holds a panel for every row the viewport can show at once,
    // each reused for every pool-th row.
    const pool = Math.ceil(viewport / height) + 1
    const shown = visible(total, height, scroll.offset, viewport)
    const first = list.first ?? 0
    const rows = list.rows ?? []
    const held = { first, end: first + rows.length }

    // The list clips a content panel as tall as all its rows, moved up by
    // the scroll.
    const content = `${id}~`
    const style: Style = {
      ...node.style,
      overflowX: Overflow.CLIP,
      overflowY: Overflow.CLIP,
    }
    nodes.set(id, {
      ...node,
      kind: Kind.PANEL,
      list: undefined,
      style,
      children: [content],
    })
    const slots: string[] = []
    nodes.set(content, {
      id: content,
      kind: Kind.PANEL,
      style: {
        width: { value: { case: 'percent', value: 100 } },
        height: pixels(total * height),
        y: pixels(-scroll.offset),
      },
      children: slots,
    })

    // Draw each shown row the host has sent from the template, in the slot
    // its place picks.
    const template = new Map(
      (list.row ?? []).map((part) => [part.id ?? '', part]),
    )
    scroll.slots.clear()
    for (let at = shown.first; at < shown.end; at++) {
      const row = at >= held.first && at < held.end ? rows[at - first] : null
      if (!row) continue
      const slot = `${id}~${at % pool}`
      slots.push(slot)
      scroll.slots.set(slot, row.key ?? '')
      const look: Style = {
        ...(row.key === scroll.focus.key ? list.focusStyle : undefined),
        ...(row.key === list.selected ? list.selectedStyle : undefined),
      }
      this.drawRow(slot, at * height, height, row, template, look, nodes)
    }

    // Ask for the rows the screen needs next, once per window, and tell the
    // host each new focus.
    if (!this.window) return
    const ask = wanted(total, held, scroll.asked, shown)
    if (ask) scroll.asked = ask
    if (!ask && scroll.focus.key === scroll.sent) return
    const span =
      scroll.asked.end > scroll.asked.first
        ? scroll.asked
        : { first, end: first + Math.max(1, rows.length) }
    scroll.sent = scroll.focus.key
    this.window(mod, id, span.first, span.end - span.first, scroll.sent)
  }

  /**
   * drawRow adds the nodes of one row drawn from the template, its root at
   * slot, placed at top and height tall, with look over the root's style.
   */
  private drawRow(
    slot: string,
    top: number,
    height: number,
    row: Row,
    template: Map<string, Node>,
    look: Style,
    nodes: Map<string, Node>,
  ): void {
    const name = (part: string) => (part === '' ? slot : `${slot}~${part}`)
    for (const [part, each] of template) {
      const filled: Node = {
        ...each,
        id: name(part),
        children: (each.children ?? []).map(name),
      }
      if (each.text) filled.text = fill(each.text, row)
      if (each.image) {
        const field = placeholder(each.image)
        if (field !== null) filled.image = row.fields?.[field] ?? ''
      }
      if (each.variables) {
        filled.variables = Object.fromEntries(
          Object.entries(each.variables).map(([key, value]) => [
            key,
            fill(value, row),
          ]),
        )
      }
      if (part === '') {
        filled.style = {
          width: { value: { case: 'percent', value: 100 } },
          ...each.style,
          ...look,
          y: pixels(top),
          height: pixels(height),
        }
      }
      nodes.set(filled.id!, filled)
    }
  }
}
