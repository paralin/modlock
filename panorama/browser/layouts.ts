// The preview page runs a layout's scripts from the page's own paths, binds
// its event attributes in its own script scope, fills its labels' dialog
// variables, and reads back the press a script sends as the console command
// actionCommand writes, to send it to modlock dev.

/** LayoutPress is a press a layout's script sent from its node. */
export interface LayoutPress {
  readonly mod: string
  readonly node: string
  readonly action: string
  readonly payload: string
}

/**
 * layoutPress reads the press in a modlockaction console command, with its
 * payload decoded to text, or returns undefined for any other command and for
 * a payload that is not base64url of UTF-8.
 */
export function layoutPress(line: string): LayoutPress | undefined {
  // Split the command into its words.
  const [name, mod, node, action, encoded, ...rest] = line.split(' ')
  if (name !== 'modlockaction' || !mod || !node || !action || rest.length) {
    return undefined
  }
  if (encoded === undefined) return { mod, node, action, payload: '' }

  // Decode the payload's base64url, then its UTF-8.
  if (!/^[\w-]+$/.test(encoded)) return undefined
  try {
    const binary = atob(encoded.replace(/-/g, '+').replace(/_/g, '/'))
    const bytes = Uint8Array.from(binary, (each) => each.charCodeAt(0))
    const payload = new TextDecoder('utf-8', { fatal: true }).decode(bytes)
    return { mod, node, action, payload }
  } catch {
    return undefined
  }
}

/**
 * scriptPath returns the page path of a script a layout includes by its game
 * path, with build's query, or '' for one the page does not serve. The layout
 * library is the page's own copy.
 */
export function scriptPath(src: string, build: string): string {
  const match =
    /^(?:s2r:\/\/panorama|file:\/\/\{resources\})\/scripts\/([^/]+\/.+)\.v?js$/.exec(
      src,
    )
  if (match?.[1] === 'modlock/layout') return '/layout.js'
  return match ? `/scripts/${match[1]}.js${build}` : ''
}

/**
 * Compile turns the code of a layout's event attribute, such as
 * onactivate="RollDice()", into a handler that runs it in the layout's script
 * scope.
 */
export type Compile = (code: string) => () => void

/**
 * LayoutScope binds the event attributes of one loaded layout, its snippets
 * included, in that layout's own script scope, so a nested layout's
 * onactivate calls its own script's functions and never the outer layout's.
 * An event bound before the layout's scripts have run waits, and the waiting
 * events bind in order when ready gives the scope; a later event binds at
 * once.
 */
export class LayoutScope {
  private compile?: Compile
  private waiting: (() => void)[] = []

  /** bind gives set the handler for code once the scope is ready. */
  bind(code: string, set: (handler: () => void) => void): void {
    if (this.compile) set(this.compile(code))
    else this.waiting.push(() => this.bind(code, set))
  }

  /**
   * ready sets the compile of the layout's script scope and binds every
   * waiting event. The scope is set once; a later call changes nothing.
   */
  ready(compile: Compile): void {
    // Keep the first scope, since the layout's scripts run once.
    if (this.compile) return
    this.compile = compile

    // Bind the events the layout's panels and snippets queued.
    const waiting = this.waiting
    this.waiting = []
    for (const each of waiting) each()
  }
}

// placePattern matches a dialog variable place in a label's text: {s:name},
// {d:name} and {f:name} for a string, integer and float, and {g:kind:name}
// for a value the game formats as kind, such as a hero's name.
const placePattern = /\{([sdf]|g:[^:}]+):([^}]+)\}/g

/**
 * fillPlaces returns source with each dialog variable place filled from the
 * value lookup gives its name. A {g:kind:name} place shows kind and the value,
 * since the page has no game text, and a place whose name has no value stays
 * as written.
 */
export function fillPlaces(
  source: string,
  lookup: (name: string) => string | undefined,
): string {
  return source.replace(placePattern, (place, kind: string, name: string) => {
    // Fill the place by its kind, or keep it when its name has no value.
    const value = lookup(name)
    if (value === undefined) return place
    if (kind === 'g:citadel_hero_name') return `hero ${value}`
    if (kind.startsWith('g:')) return `${kind.slice(2)} ${value}`
    return value
  })
}

/**
 * localize returns what the page shows for a localization token, since it has
 * no game text: the token with its places filled, or, for a token with no
 * places such as #hud_score, the token followed by the panel's own values, so
 * the preview still shows the score.
 */
export function localize(
  token: string,
  own: Readonly<Record<string, string>>,
  lookup: (name: string) => string | undefined,
): string {
  if (new RegExp(placePattern.source).test(token)) {
    return fillPlaces(token, lookup)
  }
  const values = Object.values(own)
  return values.length ? `${token} ${values.join(' ')}` : token
}
