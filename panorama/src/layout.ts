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

import {
  actionCommand,
  isRouteName,
  layoutDataAttribute,
  layoutDataChanged,
  layoutModAttribute,
  layoutNodeAttribute,
  routeAttribute,
} from './data.js'

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
  if (watches.length === 0) {
    $.RegisterForUnhandledEvent('PlaySoundEffect', (sound: string) => {
      if (sound === layoutDataChanged) {
        reread()
      }
    })
  }
  const last = panel.GetAttributeString(attribute, '')
  watches.push({ attribute, handler, last })
  handler(parse(last))
}

/** reread hands each handler whose value changed its new value. */
function reread(): void {
  for (const each of watches) {
    const text = panel.GetAttributeString(each.attribute, '')
    if (text !== each.last) {
      each.last = text
      each.handler(parse(text))
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
