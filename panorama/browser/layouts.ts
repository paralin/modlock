// The preview page runs a layout's scripts from the page's own paths, and
// reads back the press a script sends as the console command actionCommand
// writes, to send it to modlock dev.

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
