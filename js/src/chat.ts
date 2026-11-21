// Chat lines: what each player said last, shown for a few seconds beside
// their hero portrait. The game's top bar shows a fresh chat line under the
// speaker's portrait; a mod that hides the top bar loses those lines, and
// showChatLines puts them back in the HUD kit's roster at the top right.
//
// The interface library draws the lines over every player's interface, as it
// draws toasts, so the mod never redraws for them. A team line reads only to
// the speaker's team; a line for all reads to everyone. A command, such as
// /help, is for the mod and is not shown.
//
//   hideHudForAll(['topBar'])
//   showChatLines()

import { clip } from './format.js'
import type { ChatEvent, Player } from './host.gen.js'
import { Roster } from './hud.js'
import { connection } from './players.js'
import type { Child } from './ui.js'

/** ChatLinesOptions shape the lines showChatLines draws. */
export interface ChatLinesOptions {
  /** seconds is how long a line stays up; 6 unless set. */
  readonly seconds?: number
  /** top is the lines' top margin, as Roster's, to clear the mod's own HUD. */
  readonly top?: number
}

/** Line is one player's latest chat line. */
interface Line {
  readonly speaker: Player
  /** text is the line as shown: quoted and clipped. */
  readonly text: string
  /** team is the speaker's team for a team line, or 0 for a line to all. */
  readonly team: number
  /** until is when the line leaves, in game seconds. */
  readonly until: number
}

/** lineWidth is the most characters of a line shown. */
const lineWidth = 40

/** lines holds each speaker's fresh line, by slot. */
const lines = new Map<number, Line>()

/** shown holds the options while the mod shows chat lines. */
let shown: ChatLinesOptions | undefined

/**
 * showChatLines shows each player's chat lines beside their hero portrait at
 * the top right, for every player, for as long as the mod runs. Call it when
 * the mod loads, as with onChat, so the server sends the mod its chat.
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
  const team = line.all ? 0 : (line.player.pawn()?.team ?? 0)
  lines.set(line.player.slot, {
    speaker: line.player,
    text: `“${clip(text, lineWidth)}”`,
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

/** chatLines draws the fresh lines viewer may read, newest at the top. */
export function chatLines(viewer: Player): Child {
  // Nothing to draw is the common case.
  if (lines.size === 0) return undefined

  // Keep the lines viewer may read, newest first.
  const team = viewer.pawn()?.team ?? 0
  const readable: Line[] = []
  for (const line of lines.values()) {
    if (line.team === 0 || line.team === team) readable.push(line)
  }
  readable.sort((a, b) => b.until - a.until)

  // Each line is a roster row: portrait, name and the line under it.
  return Roster({
    top: shown?.top,
    rows: readable.map((line) => {
      const speaker = connection(line.speaker)
      return {
        name: speaker?.name ?? '',
        hero: line.speaker.pawn()?.hero ?? 0,
        activity: line.text,
        steamId: speaker && !speaker.bot ? speaker.steamId : undefined,
      }
    }),
  })
}
