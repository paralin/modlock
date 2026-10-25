// The HUD kit's tokens, each named once: the colors, type scale and sizes the
// kit's components draw with, and that a mod's own panels can share. They follow
// the stock HUD's look: VALVEPulp capitals and Retail Demo text in off-white
// with a dark shadow, team colors and translucent black plates.

/** Tone names the colors a surface may use for meaning. */
export type Tone =
  | 'text'
  | 'muted'
  | 'accent'
  | 'onAccent'
  | 'amber'
  | 'sapphire'
  | 'gas'
  | 'success'
  | 'danger'
  | 'gold'

/**
 * tones maps each meaning to one color. Text, team and danger colors are the
 * stock HUD's offWhite, team1Color, team2Color and colorEnemy.
 */
export const tones: Record<Tone, string> = {
  text: '#ffefd7',
  muted: '#bfb3a1',
  // The accent marks the player's own choices.
  accent: '#fd736d',
  // Text on an accent fill.
  onAccent: '#070709',
  amber: '#d4860b',
  sapphire: '#4d75c3',
  // The gas of a closing arena.
  gas: '#3ee08f',
  // Done, under par, ready and live: a step darker than gas, so status text
  // and tags sit calmer on the plates.
  success: '#36c47f',
  danger: '#ff410d',
  gold: '#ebc934',
}

/**
 * teamTones colors each game team, by its team number: 2 is the Hidden
 * King's amber, 3 the Archmother's sapphire.
 */
export const teamTones: Readonly<Record<number, Tone>> = {
  2: 'amber',
  3: 'sapphire',
}

/**
 * shades are the neutrals a full screen draws with, from the stock main menu's
 * base styles, so a full screen reads as part of the game's own menus. Fills are
 * opaque offWhite tints of the offBlack backdrop: Panorama blends translucent
 * fills far stronger than their alpha, which let the stock menu show through.
 */
export const shades = {
  /** offWhite at about half strength: idle navigation and secondary text. */
  quiet: '#ffefd78c',
  /** offWhite at the popup button's 44%: idle pill borders. */
  faint: '#ffefd770',
  /** offWhite at about 40%: counts, placeholders and offline players. */
  dim: '#6b675d',
  /** offWhite at 13%: hairlines between sections and plate edges. */
  hairline: '#2b2a24',
  /** offBlack: text on light fills, and the backdrop's hue. */
  ink: '#10130d',
  /** The menu backdrop: an opaque offBlack that hides the stock menu. */
  backdrop: '#0b0d09',
  /** The backdrop at 95%, under a panel the player reads over the game. */
  veil: '#0b0d09f2',
  /** offWhite at 4%: a plate on the backdrop. */
  plate: '#151611',
  /** offWhite at 8%: a row or stat inside a plate. */
  raised: '#1e1f19',
  /** offWhite at 13%: a hovered or joined row. */
  lit: '#2b2a24',
  /** The full-strength fill of a lit primary button. */
  white: '#ffffff',
  /** No fill or border, where a state turns one off. */
  clear: '#00000000',
} as const

/**
 * sizes holds the shared sizes: panel buttons and fields share one height, row
 * buttons a shorter one, and every control and tag one corner radius.
 */
export const sizes = {
  /** control is the height of panel buttons and fields. */
  control: 32,
  /** rowControl is the height of a button inside a list row. */
  rowControl: 28,
  /** row is the height of one list row. */
  row: 32,
  /** radius rounds buttons, fields and tags. */
  radius: 4,
  /** panelRadius rounds panels. */
  panelRadius: 8,
} as const

/**
 * Face is one of the game's typefaces: body is Retail Demo, its text; block is
 * VALVEPulp, set in capitals for the stock clock, round titles and callouts;
 * serif is Reaver, its menus.
 */
export type Face = 'body' | 'block' | 'serif'

/** TextRole names one step of the HUD type scale. */
export type TextRole = 'display' | 'title' | 'body' | 'label' | 'caption'

/** Type is one type role: its face, size in pixels, and whether in capitals. */
export interface Type {
  readonly face: Face
  readonly size: number
  readonly uppercase: boolean
}

/** types gives every build the same typography. */
export const types: Record<TextRole, Type> = {
  display: { face: 'block', size: 46, uppercase: true },
  title: { face: 'block', size: 26, uppercase: true },
  body: { face: 'body', size: 17, uppercase: false },
  label: { face: 'block', size: 15, uppercase: true },
  caption: { face: 'body', size: 14, uppercase: false },
}

/**
 * plate is the translucent plate under HUD text. It stays dark enough for
 * muted captions to read over a bright sky.
 */
export const plate = {
  color: '#000000e0',
  radius: 5,
  /** padding is [vertical, horizontal] in pixels. */
  padding: [10, 18] as const,
}

/** pillEdge is a pill's resting edge, a faint parchment line. */
export const pillEdge = '#ffefd71a'

/** PipShape is a round point, as in the stock round counter, or a narrow tick. */
export type PipShape = 'round' | 'tick'

/** pipShapes are each pip shape's size and corner radius, in pixels. */
export const pipShapes: Record<
  PipShape,
  { readonly width: number; readonly height: number; readonly radius: number }
> = {
  round: { width: 18, height: 18, radius: 9 },
  tick: { width: 7, height: 18, radius: 2 },
}

/** pipRings are the stock round counter's point borders, empty and filled. */
export const pipRings = { empty: '#ffefd72e', filled: '#10130de6' } as const

/** emptyPip is an unearned pip's fill: clear, so only its faint ring shows. */
export const emptyPip = '#00000000'

/**
 * teamBarBottom is where the stock team bar ends, in pixels on a 1080 high
 * HUD; a strip that keeps the bar sits below it.
 */
export const teamBarBottom = 84

/**
 * objectiveBarBottom is where the stock objective bar under the team bar ends,
 * its urn and Rift progress, in pixels on a 1080 high HUD; a strip's line goes
 * below it so the bar's labels stay clear.
 */
export const objectiveBarBottom = 220

/**
 * fpsReadoutBottom is how far down the top right corner HUD starts, in pixels
 * on a 1080 high HUD: under the game's two line FPS readout, which ends about
 * 41px down.
 */
export const fpsReadoutBottom = 48
