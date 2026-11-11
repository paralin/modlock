// The data contract between the renderer and a mod's layout scripts: the
// attributes of a layout's panel that hold its data, the mod's routes and
// the names a press goes back with, the event that announces a change, and
// the console command a layout's press runs. The renderer writes them and the
// layout library reads them, so only these two change when the contract does.

/** layoutDataAttribute names the attribute holding a layout node's data. */
export const layoutDataAttribute = 'modlock_data'

/**
 * routeAttribute names the attribute of each layout's panel that holds the
 * route name's value, empty when the mod sends none.
 */
export function routeAttribute(name: string): string {
  return `modlock_route_${name}`
}

/**
 * layoutDataChanged is the sound event the drawing plays after a draw that
 * changes a layout's data or a route. The game refuses custom events and
 * delivers the stock PlaySoundEffect to every script; no sound has this
 * name, so none plays.
 */
export const layoutDataChanged = 'modlock_data_changed'

/**
 * isRouteName reports whether name names a route: one or more letters,
 * digits, '_', '-' and '.', so it fits in an attribute's name.
 */
export function isRouteName(name: string): boolean {
  return /^[\w.-]+$/.test(name)
}

/**
 * layoutModAttribute names the attribute holding the name of the running mod
 * instance whose tree shows the layout. That name is the mod's slug unless
 * the host loaded the mod under another name.
 */
export const layoutModAttribute = 'modlock_mod'

/** layoutNodeAttribute names the attribute holding a layout node's id. */
export const layoutNodeAttribute = 'modlock_node'

/**
 * maxActionPayload is the most bytes of UTF-8 a layout's press carries, so
 * its console command stays inside the engine's line.
 */
export const maxActionPayload = 180

/**
 * actionCommand is the console command that sends a layout's press to its
 * mod: the action's name and payload's text, as unpadded base64url of its
 * UTF-8, which an empty payload leaves off. The game server hears it with the sending player, as it hears a
 * button's press, and the host refuses it unless that player's tree shows the
 * layout. It is empty for a name, a node or a mod the host would refuse, and
 * for a payload over maxActionPayload bytes.
 */
export function actionCommand(
  mod: string,
  node: string,
  action: string,
  payload: string,
): string {
  // Encoding past the limit by a character's most bytes shows an overflow.
  const bytes = utf8(payload, maxActionPayload + 4)
  if (
    !/^[a-z0-9]+(-[a-z0-9]+)*$/.test(mod) ||
    !/^[\w.:-]{1,64}$/.test(node) ||
    !/^[\w.-]{1,64}$/.test(action) ||
    bytes.length > maxActionPayload
  ) {
    return ''
  }
  const command = `modlockaction ${mod} ${node} ${action}`
  return bytes.length === 0 ? command : `${command} ${base64Url(bytes)}`
}

/**
 * utf8 returns text's UTF-8 bytes, cut at a character to at most limit bytes.
 * A lone surrogate becomes the replacement character.
 */
export function utf8(text: string, limit: number): number[] {
  const bytes: number[] = []
  for (const character of text) {
    // Encode one character in one to four bytes.
    let point = character.codePointAt(0) ?? 0xfffd
    if (point >= 0xd800 && point <= 0xdfff) {
      point = 0xfffd
    }
    const tail = (shift: number) => 0x80 | ((point >> shift) & 63)
    const encoded =
      point < 0x80
        ? [point]
        : point < 0x800
          ? [0xc0 | (point >> 6), tail(0)]
          : point < 0x10000
            ? [0xe0 | (point >> 12), tail(6), tail(0)]
            : [0xf0 | (point >> 18), tail(12), tail(6), tail(0)]

    // Stop before the character that would pass the limit.
    if (bytes.length + encoded.length > limit) {
      break
    }
    bytes.push(...encoded)
  }
  return bytes
}

/** base64Digits are the digits of base64url in order. */
const base64Digits =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'

/** base64Url writes bytes in unpadded base64url. */
export function base64Url(bytes: readonly number[]): string {
  let written = ''
  for (let at = 0; at < bytes.length; at += 3) {
    // Each three bytes make four digits; a short end makes one per byte and one.
    const chunk =
      (bytes[at]! << 16) | ((bytes[at + 1] ?? 0) << 8) | (bytes[at + 2] ?? 0)
    const digits = Math.min(3, bytes.length - at) + 1
    for (let digit = 0; digit < digits; digit++) {
      written += base64Digits[(chunk >> (18 - 6 * digit)) & 63]
    }
  }
  return written
}
