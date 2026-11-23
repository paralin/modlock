// The Modlock layout library gives a mod's layout script the values the mod
// sends it. A layout includes it before its own script:
//
//   <scripts>
//     <include src="file://{resources}/scripts/modlock/layout.js" />
//     <include src="file://{resources}/scripts/{addon}/shop.js" />
//   </scripts>
//
// and its script reads its element's data and the mod's routes:
//
//   Modlock.data((stock) => drawShop(stock))
//   Modlock.route('scores', (scores) => drawScores(scores))
//
// Each handler runs once at once with the current value, then after each
// change, with undefined while the mod sends none. The library reads the
// attributes the renderer sets on the layout's panel and rereads them on the
// renderer's change event, so no layout script names either.
//
// A layout sends a press back to its mod's server with a name and a small
// JSON payload:
//
//   Modlock.press('buy', { item: 'boots' })
//
// The mod hears it as a press on the layout's node with the action and the
// payload's JSON, from the player the server says sent it.
//
// A layout cleans up before its panel leaves:
//
//   Modlock.onUnload(() => cancelTimers())
//
// Each handler runs once when the renderer starts removing or replacing the
// layout or releases the screen, and no data or route handler runs after it.

import {
  actionCommand,
  isRouteName,
  layoutDataAttribute,
  layoutDataChanged,
  layoutModAttribute,
  layoutNodeAttribute,
  layoutUnloadingAttribute,
  routeAttribute,
} from './data.js'

/**
 * $ is the part of Panorama's script API this library calls. Declaring it here
 * keeps the library self-contained, so a consumer that compiles this file from
 * a vendored copy needs no global declarations of its own.
 */
declare const $: {
  Msg(...values: unknown[]): void
  DispatchEvent(event: string, ...args: unknown[]): void
  GetContextPanel(): {
    GetAttributeString(name: string, fallback: string): string
    IsValid?(): boolean
  }
  RegisterForUnhandledEvent(
    event: string,
    handler: (...args: never[]) => boolean | void,
  ): number
}

/** Handler receives a value the mod sent, or undefined for none. */
export type Handler = (value: unknown) => void

/** Watch is one handler and the text of the value it last received. */
interface Watch {
  readonly attribute: string
  readonly handler: Handler
  last: string
}

/** panel is the layout's panel, which holds the attributes. */
const panel = $.GetContextPanel()

/** watches holds every handler the layout registered. */
const watches: Watch[] = []

/** cleanups holds every unload handler the layout registered. */
const cleanups: (() => void)[] = []

/** listening records whether the PlaySoundEffect listener is registered. */
let listening = false

/** unloaded records that the layout's unload handlers have run. */
let unloaded = false

/**
 * data calls handler with the layout element's data now and after each
 * change.
 */
export function data(handler: Handler): void {
  watch(layoutDataAttribute, handler)
}

/**
 * route calls handler with the value of the mod's route name now and after
 * each change. A name the renderer cannot carry never receives a value.
 */
export function route(name: string, handler: Handler): void {
  if (!isRouteName(name)) {
    $.Msg(`[Modlock] ${name} cannot name a route`)
  }
  watch(routeAttribute(name), handler)
}

/**
 * onUnload runs handler once when the renderer starts removing or replacing
 * the layout's panel or releases the screen, so the script can cancel timers
 * or undo changes outside the panel.
 */
export function onUnload(handler: () => void): void {
  cleanups.push(handler)
  if (leaving()) {
    unload()
    return
  }
  listen()
}

/**
 * press sends action to the layout's mod with payload as JSON, and reports
 * whether it went: a name the host refuses, a payload whose JSON passes the
 * bytes a press carries, or a layout the renderer did not name stays here.
 * The host takes a few presses a second from each player and drops the rest.
 */
export function press(action: string, payload?: unknown): boolean {
  // Build the command from the node the renderer named on this panel.
  const json = payload === undefined ? '' : (JSON.stringify(payload) ?? '')
  const command = actionCommand(
    panel.GetAttributeString(layoutModAttribute, ''),
    panel.GetAttributeString(layoutNodeAttribute, ''),
    action,
    json,
  )
  if (command === '') {
    $.Msg(`[Modlock] ${action} cannot be pressed`)
    return false
  }
  $.DispatchEvent('CitadelConCommand', command)
  return true
}

/** watch registers handler for attribute and hands it the current value. */
function watch(attribute: string, handler: Handler): void {
  // Ignore new watches once this panel is leaving.
  if (leaving()) {
    return
  }

  // Listen for changes, record the current value and hand it over now.
  listen()
  const last = panel.GetAttributeString(attribute, '')
  watches.push({ attribute, handler, last })
  handler(parse(last))
}

/** listen registers once for the renderer's change and unload event. */
function listen(): void {
  if (listening) {
    return
  }
  listening = true
  $.RegisterForUnhandledEvent('PlaySoundEffect', (sound: string) => {
    if (sound === layoutDataChanged) {
      reread()
    }
  })
}

/** reread runs unload handlers when leaving, or hands changes to watches. */
function reread(): void {
  if (unloaded) {
    return
  }
  if (leaving()) {
    unload()
    return
  }
  for (const each of watches) {
    const text = panel.GetAttributeString(each.attribute, '')
    if (text !== each.last) {
      each.last = text
      each.handler(parse(text))
    }
  }
}

/** leaving reports whether this layout is already unloading or gone. */
function leaving(): boolean {
  return unloaded || panel.IsValid?.() === false || unloading()
}

/** unloading reports whether the renderer marked this panel as leaving. */
function unloading(): boolean {
  return panel.GetAttributeString(layoutUnloadingAttribute, '') === '1'
}

/** unload stops data callbacks and runs each cleanup handler once. */
function unload(): void {
  unloaded = true
  watches.length = 0
  for (const handler of cleanups.splice(0)) {
    try {
      handler()
    } catch (error) {
      $.Msg(`[Modlock] layout unload failed: ${String(error)}`)
    }
  }
}

/** parse reads a value's JSON, or undefined for none or for text JSON cannot read. */
function parse(text: string): unknown {
  if (text === '') {
    return undefined
  }
  try {
    return JSON.parse(text) as unknown
  } catch {
    return undefined
  }
}
