// Chat lines: what players just said, shown as subtitles low in the middle of
// the screen. The game's top bar shows a fresh chat line under the speaker's
// portrait; a mod that hides the top bar loses those lines, and showChatLines
// puts them back where the eye already rests: centred above the ability bar,
// clear of the crosshair, newest at the bottom.
//
// Each line is the speaker's portrait, their name in their team's color and
// the line itself, on a slim, translucent band. At most four show; a
// speaker's next line replaces their last, and older lines dim. A team line reads only to the
// speaker's team; a line for all reads to everyone. A command, such as /help,
// is for the mod and is not shown.
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
   * a 1080 screen; unless set, it sits just above the ability bar.
   */
  readonly bottom?: number
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

  // The lane is centred above the ability bar; each line keeps its speaker's
  // key, so a new line adds one subtitle and dims the rest in place.
  return {
    type: 'panel',
    props: {
      style: {
        flow: 'down',
        horizontalAlign: 'center',
        verticalAlign: 'bottom',
        margin: [0, 0, shown.bottom ?? abilityBarTop, 0],
      },
      children: kept.map((line, at) =>
        subtitle(line, viewer, kept.length - 1 - at),
      ),
    },
  }
}

/**
 * abilityBarTop is the ability bar's top on a 1080 screen, with room for its
 * upgrade pips, from the bottom edge.
 */
const abilityBarTop = 190

/**
 * bandWidth is the widest a subtitle's band grows, centred on a 1080 screen:
 * its left edge, at 640, stays well clear of the stamina meter, which ends
 * at 530 left of the crosshair. The name and the line share it, and a longer
 * line wraps inside it.
 */
const bandWidth = 640

/** nameWidth is the most of the band a speaker's name takes. */
const nameWidth = 180

/**
 * lineWidth is the band left for the line: the band less its padding, the
 * portrait, the name and their gaps.
 */
const lineWidth = bandWidth - (4 + 10) - 24 - 7 - nameWidth - 9

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
  lineHeight: 23,
  textAlign: 'left',
}

/** portraits finds each hero's portrait by id. */
const portraits = new Map(heroes.map((hero) => [hero.id, hero.portrait]))

/**
 * subtitle is one line as viewer sees it: the portrait, the name and the
 * text at 18 pixels on a slim, translucent band, dimmer with age.
 */
function subtitle(line: Line, viewer: Player, age: number): UiElement {
  // The speaker's look: the mod's say, else their hero and team.
  const pawn = line.speaker.pawn()
  const seen = shown?.speaker?.(line.speaker, viewer) ?? {}
  const tone = teamTones[pawn?.team ?? 0]
  const color = seen.out
    ? shades.quiet
    : (seen.color ?? (tone ? lifted[tone] : undefined) ?? tones.text)
  const notes = [line.team !== 0 ? 'team' : '', seen.note ?? '']
    .filter((note) => note !== '')
    .map((note) => ` (${note})`)
    .join('')
  const name = `${connection(line.speaker)?.name ?? ''}${notes}`
  const src = portraits.get(seen.hero ?? pawn?.hero ?? 0)

  // The portrait sits on a raised square, empty for a hidden or missing hero.
  const portrait: UiElement = {
    type: 'panel',
    props: {
      style: {
        width: 24,
        height: 24,
        verticalAlign: 'top',
        borderRadius: 2,
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
        fontSize: 16,
        color,
        margin: [1, 0, 0, 7],
        maxWidth: nameWidth,
      },
      children: name,
    },
  }
  const said: UiElement = {
    type: 'label',
    props: {
      style: {
        ...words,
        fontSize: 18,
        color: tones.text,
        margin: [0, 0, 0, 9],
        maxWidth: lineWidth,
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
        borderRadius: 3,
        padding: [3, 10, 4, 4],
        margin: [0, 0, 4, 0],
        opacity: fades[age] ?? fades[fades.length - 1],
        transition: { seconds: 0.25, easing: 'out' },
      },
      enter: { opacity: 0, translate: [0, 8] },
      exit: { opacity: 0 },
      children: [portrait, speaker, said],
    },
  }
}
