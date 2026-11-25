// Chat lines: what players just said, shown as subtitles low in the middle of
// the screen. The game's top bar shows a fresh chat line under the speaker's
// portrait; a mod that hides the top bar loses those lines, and showChatLines
// puts them back where the eye already rests: centred a little above the
// ability bar, clear of the crosshair, newest at the bottom.
//
// Each line is the speaker's portrait, their name in their team's color and
// the line itself, on a slim, translucent band. At most four show; a
// speaker's next line replaces their last, and older lines dim. A team line
// reads only to the speaker's team; a line for all reads to everyone. A
// command, such as /help, is for the mod and is not shown. The lines give way
// while the player has the game's chat box open, which shows the chat history
// itself.
//
// The interface library draws the lines as their own layer of every player's
// interface, so a line arriving or leaving redraws only the lines, never the
// mod's own tree.
//
//   hideHudForAll(['topBar'])
//   showChatLines()

import { heroes } from './heroes.js'
import type { ChatEvent, Player } from './host.gen.js'
import { connection } from './players.js'
import { shades, teamTones, tones } from './theme.js'
import type { Child, Style, UiElement } from './ui.js'

/** ChatLinesOptions shape the lines showChatLines draws. */
export interface ChatLinesOptions {
  /** seconds is how long a line stays up; 6 unless set. */
  readonly seconds?: number
  /** lines is the most lines shown at once, newest kept; 4 unless set. */
  readonly lines?: number
  /**
   * bottom is the lane's distance from the bottom of the screen, in pixels on
   * a 1080 screen; unless set, it sits a little above the ability bar.
   */
  readonly bottom?: number
  /**
   * scale sizes each line, its text, portrait, band and spacing together,
   * against the default of 1: 0.8 draws the text at 13 pixels on a band at
   * most 448 wide. Sizes round to whole pixels.
   */
  readonly scale?: number
  /** portraits shows each speaker's portrait before their name; true unless set. */
  readonly portraits?: boolean
  /**
   * speaker says how viewer sees a line's speaker, for a mode whose sides are
   * not the game's teams or that keeps a hero secret. Unset fields, and an
   * unset speaker, keep the defaults: the speaker's hero, and their name in
   * their game team's color.
   */
  readonly speaker?: (speaker: Player, viewer: Player) => ChatSpeaker
}

/** ChatSpeaker is how one viewer sees a line's speaker. */
export interface ChatSpeaker {
  /** color is the name's color, as a style's; a light color reads best. */
  readonly color?: string
  /** hero is the portrait's hero, 0 for an empty square. */
  readonly hero?: number
  /** note follows the name in brackets, such as 'found'. */
  readonly note?: string
  /** out greys the portrait and the name, as for a player knocked out. */
  readonly out?: boolean
}

/** Line is one player's latest chat line. */
interface Line {
  readonly speaker: Player
  readonly text: string
  /** team is the speaker's team for a team line, or 0 for a line to all. */
  readonly team: number
  /** until is when the line leaves, in game seconds. */
  readonly until: number
}

/**
 * lines holds each speaker's fresh line by slot, oldest first: a speaker's
 * new line goes to the end.
 */
const lines = new Map<number, Line>()

/** shown holds the options while the mod shows chat lines. */
let shown: ChatLinesOptions | undefined

/**
 * showChatLines shows chat lines as subtitles above the ability bar, for
 * every player, for as long as the mod runs. Call it when the mod loads, as
 * with onChat, so the server sends the mod its chat.
 */
export function showChatLines(options: ChatLinesOptions = {}): void {
  shown = options
}

/** chatShown reports whether the mod shows chat lines. */
export function chatShown(): boolean {
  return shown !== undefined
}

/**
 * hearChat keeps line, written at now, and reports whether the lines changed.
 * It keeps nothing while the mod shows no chat lines.
 */
export function hearChat(line: ChatEvent, now: number): boolean {
  // Commands and empty lines are not shown.
  if (!shown) return false
  const text = line.text.trim()
  if (text === '' || text.startsWith('/')) return false

  // A team line reads only to the team the speaker is on as they write it.
  // The speaker's line moves to the end, as the newest.
  const team = line.all ? 0 : (line.player.pawn()?.team ?? 0)
  lines.delete(line.player.slot)
  lines.set(line.player.slot, {
    speaker: line.player,
    text,
    team,
    until: now + (shown.seconds ?? 6),
  })
  return true
}

/** expireChat drops the lines past their time and reports whether any went. */
export function expireChat(now: number): boolean {
  let expired = false
  for (const [slot, line] of lines) {
    if (now >= line.until) {
      lines.delete(slot)
      expired = true
    }
  }
  return expired
}

/** forgetChat drops the line of the player who left slot, reporting whether one went. */
export function forgetChat(slot: number): boolean {
  return lines.delete(slot)
}

/** chatLines draws the fresh lines viewer may read, newest at the bottom. */
export function chatLines(viewer: Player): Child {
  // Nothing to draw is the common case.
  if (lines.size === 0 || !shown) return undefined

  // Keep the newest lines viewer may read, oldest first.
  const team = viewer.pawn()?.team ?? 0
  const readable: Line[] = []
  for (const line of lines.values()) {
    if (line.team === 0 || line.team === team) readable.push(line)
  }
  if (readable.length === 0) return undefined
  const kept = readable.slice(-(shown.lines ?? 4))
  const size = sizesFor(shown.scale ?? 1, shown.portraits ?? true)

  // The lane is centred above the ability bar and gives way to the game's
  // chat box; each line keeps its speaker's key, so a new line adds one
  // subtitle and dims the rest in place.
  return {
    type: 'panel',
    props: {
      hideInChat: true,
      style: {
        flow: 'down',
        horizontalAlign: 'center',
        verticalAlign: 'bottom',
        margin: [0, 0, shown.bottom ?? laneBottom, 0],
      },
      children: kept.map((line, at) =>
        subtitle(line, viewer, kept.length - 1 - at, size),
      ),
    },
  }
}

/**
 * laneBottom is the lane's distance from the bottom of a 1080 screen: the
 * ability bar's top, with room for its upgrade pips, at 190, and a gap of 40
 * above it, so the lines read apart from the bar.
 */
const laneBottom = 230

/**
 * Sizes are a subtitle's measures at a scale, in pixels on a 1080 screen.
 * At scale 1 the band grows at most 560 wide, centred: its left edge, at 680,
 * stays well clear of the stamina meter, which ends at 530 left of the
 * crosshair. The name and the line share it, and a longer line wraps inside
 * it, in the width left after the padding, the portrait, the name and their
 * gaps.
 */
interface Sizes {
  readonly text: number
  readonly name: number
  readonly lineHeight: number
  /** portrait is the side of the speaker's square portrait, 0 for none. */
  readonly portrait: number
  /** nameWidth is the most of the band a speaker's name takes. */
  readonly nameWidth: number
  /** lineWidth is the most of the band the line takes before it wraps. */
  readonly lineWidth: number
  /** pad is the band's padding: top, right, bottom, left. */
  readonly pad: readonly [number, number, number, number]
  /** gap is the space after the portrait, then after the name. */
  readonly gap: readonly [number, number]
  /** space is the gap between lines. */
  readonly space: number
  readonly radius: number
}

/** sizesFor is the subtitle's measures at scale, with or without portraits. */
function sizesFor(scale: number, portraits: boolean): Sizes {
  // Each measure at scale 1, scaled and rounded; without a portrait the band
  // pads its left as its right and drops the portrait's gap.
  const px = (n: number) => Math.max(1, Math.round(n * scale))
  const portrait = portraits ? px(20) : 0
  const pad = [px(2), px(8), px(3), portraits ? px(3) : px(8)] as const
  const gap = [portraits ? px(6) : 0, px(7)] as const
  const nameWidth = px(160)
  return {
    text: px(16),
    name: px(14),
    lineHeight: px(20),
    portrait,
    nameWidth,
    lineWidth:
      px(560) - pad[1] - pad[3] - portrait - gap[0] - nameWidth - gap[1],
    pad,
    gap,
    space: px(3),
    radius: px(3),
  }
}

/** fades is each line's opacity by its age, newest first. */
const fades = [1, 0.85, 0.7]

/** lifted is each game team's name color, its tone lifted to read on the band. */
const lifted: Readonly<Record<string, string>> = {
  amber: '#f0a73a',
  sapphire: '#8fb1f0',
}

/** words is the shared style of a subtitle's name and line. */
const words: Style = {
  face: 'body',
  weight: 'bold',
  verticalAlign: 'top',
  textAlign: 'left',
}

/** portraits finds each hero's portrait by id. */
const portraits = new Map(heroes.map((hero) => [hero.id, hero.portrait]))

/** SpeakerLook is a speaker as one viewer sees them, ready to draw. */
export interface SpeakerLook {
  /** name is the player's name, with the mod's note in brackets. */
  readonly name: string
  /** color is the name's color. */
  readonly color: string
  /** src is the portrait's image, undefined for an empty square. */
  readonly src: string | undefined
  /** out greys the portrait and the name. */
  readonly out: boolean
}

/**
 * speakerLook is how viewer sees speaker: what the mod's see says, else
 * their hero, and their name in their game team's color lifted to read on a
 * dark band.
 */
export function speakerLook(
  speaker: Player,
  viewer: Player,
  see?: (speaker: Player, viewer: Player) => ChatSpeaker,
): SpeakerLook {
  // The mod's say comes first; a name with no color of its own takes the
  // team's, and an out speaker is greyed.
  const pawn = speaker.pawn()
  const seen = see?.(speaker, viewer) ?? {}
  const tone = teamTones[pawn?.team ?? 0]
  const color = seen.out
    ? shades.quiet
    : (seen.color ?? (tone ? lifted[tone] : undefined) ?? tones.text)
  const note = seen.note ? ` (${seen.note})` : ''
  return {
    name: `${connection(speaker)?.name ?? ''}${note}`,
    color,
    src: portraits.get(seen.hero ?? pawn?.hero ?? 0),
    out: seen.out ?? false,
  }
}

/**
 * subtitle is one line as viewer sees it: the portrait, the name and the
 * text, 16 pixels at scale 1, on a slim, translucent band, dimmer with age.
 */
function subtitle(
  line: Line,
  viewer: Player,
  age: number,
  size: Sizes,
): UiElement {
  // The speaker's look, with a team line noted.
  const seen = speakerLook(line.speaker, viewer, shown?.speaker)
  const name = line.team !== 0 ? `${seen.name} (team)` : seen.name
  const { color, src } = seen

  // The portrait sits on a raised square, empty for a hidden or missing hero.
  const portrait: UiElement = {
    type: 'panel',
    props: {
      style: {
        width: size.portrait,
        height: size.portrait,
        verticalAlign: 'top',
        borderRadius: Math.max(1, size.radius - 1),
        background: shades.raised,
        saturation: seen.out ? 0 : 1,
      },
      // A ternary to undefined, not a && to false: the WebAssembly build traps
      // on `src !== undefined && { ... }` here, as a value not representable
      // in the child union, where QuickJS draws it.
      children:
        src === undefined
          ? undefined
          : {
              type: 'image',
              props: { src, style: { width: '100%', height: '100%' } },
            },
    },
  }

  // The name and the line, which wraps inside the band.
  const speaker: UiElement = {
    type: 'label',
    props: {
      style: {
        ...words,
        fontSize: size.name,
        lineHeight: size.lineHeight,
        color,
        margin: [1, 0, 0, size.gap[0]],
        maxWidth: size.nameWidth,
      },
      children: name,
    },
  }
  const said: UiElement = {
    type: 'label',
    props: {
      style: {
        ...words,
        fontSize: size.text,
        lineHeight: size.lineHeight,
        color: tones.text,
        margin: [0, 0, 0, size.gap[1]],
        maxWidth: size.lineWidth,
      },
      children: line.text,
    },
  }
  return {
    type: 'panel',
    key: `line-${line.speaker.slot}`,
    props: {
      style: {
        flow: 'right',
        horizontalAlign: 'center',
        background: '#10130d9e',
        borderRadius: size.radius,
        padding: [...size.pad],
        margin: [0, 0, size.space, 0],
        opacity: fades[age] ?? fades[fades.length - 1],
        transition: { seconds: 0.25, easing: 'out' },
      },
      enter: { opacity: 0, translate: [0, 8] },
      exit: { opacity: 0 },
      children: size.portrait > 0 ? [portrait, speaker, said] : [speaker, said],
    },
  }
}
