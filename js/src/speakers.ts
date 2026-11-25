// The speaking stack: who the player hears on voice chat, as a column of
// portraits and names at the left edge of the screen. The game's top bar
// marks each player it plays a voice for; a mod that hides the top bar loses
// those marks, and showSpeakers puts them back, as showChatLines puts back
// the chat lines the top bar showed.
//
// The library draws one hidden row for each player in the game, and the
// player's game shows a row while it plays that player's voice (an element's
// speaking prop), so voices starting and stopping send nothing. Rows keep slot
// order, so a stack never reorders as people talk over each other. The stack
// draws again only when a row's look changes: a player joins, leaves, picks a
// hero or, through the mod's speaker, changes side.
//
//   hideHudForAll(['topBar'])
//   showChatLines()
//   showSpeakers()

import { type ChatSpeaker, speakerLook } from './chat.js'
import type { Player } from './host.gen.js'
import { players } from './players.js'
import { tones } from './theme.js'
import type { Child, UiElement } from './ui.js'

/** SpeakersOptions shape the stack showSpeakers draws. */
export interface SpeakersOptions {
  /**
   * scale sizes each row, its portrait, name and spacing together, against
   * the default of 1, at which the portrait is 28 pixels on a 1080 screen.
   */
  readonly scale?: number
  /**
   * speaker says how viewer sees a row's speaker, as showChatLines' speaker
   * does; pass the same one so a player looks the same in both.
   */
  readonly speaker?: (speaker: Player, viewer: Player) => ChatSpeaker
}

/** shown holds the options while the mod shows the stack. */
let shown: SpeakersOptions | undefined

/** drawn holds each viewer's last stack, as its looks, by slot. */
const drawn = new Map<number, string>()

/**
 * showSpeakers shows who each player hears on voice chat, at the left edge
 * of their screen, for as long as the mod runs.
 */
export function showSpeakers(options: SpeakersOptions = {}): void {
  shown = options
}

/**
 * speakersChanged reports whether viewer's stack looks different from the
 * one last drawn for them, and notes it as drawn.
 */
export function speakersChanged(viewer: Player): boolean {
  // The looks as one string, compared with the last drawn.
  if (!shown) return false
  const looks = rows(viewer)
    .map((row) => `${row.slot}:${row.name}:${row.color}:${row.src}:${row.out}`)
    .join('|')
  if (drawn.get(viewer.slot) === looks) return false
  drawn.set(viewer.slot, looks)
  return true
}

/** forgetSpeakers drops what was drawn for the player who left slot. */
export function forgetSpeakers(slot: number): void {
  drawn.delete(slot)
}

/** Row is one speaker's row as a viewer sees it. */
interface Row {
  readonly slot: number
  readonly player: Player
  readonly name: string
  readonly color: string
  readonly src: string | undefined
  readonly out: boolean
}

/** rows are the players viewer may hear, as viewer sees them, in slot order. */
function rows(viewer: Player): Row[] {
  const list: Row[] = []
  for (const c of players()) {
    if (c.bot) continue
    const look = speakerLook(c.player, viewer, shown?.speaker)
    list.push({ slot: c.player.slot, player: c.player, ...look })
  }
  return list
}

/**
 * speakers draws viewer's stack: a hidden row for each player, which the
 * player's game shows while it plays that player's voice.
 */
export function speakers(viewer: Player): Child {
  // Nobody to hear draws nothing; each measure scales with the stack.
  if (!shown) return undefined
  const list = rows(viewer)
  if (list.length === 0) return undefined
  const px = (n: number) => Math.max(1, Math.round(n * (shown?.scale ?? 1)))

  return {
    type: 'panel',
    props: {
      style: {
        flow: 'down',
        horizontalAlign: 'left',
        verticalAlign: 'center',
        margin: [0, 0, px(160), px(24)],
      },
      children: list.map((row) => speakerRow(row, px)),
    },
  }
}

/** voiceIcon is the stock top bar's voice chat icon. */
const voiceIcon = 'file://{images}/icons/voice_chat_icon.psd'

/** speakerRow is one row: the portrait in a green ring, the name and the icon. */
function speakerRow(row: Row, px: (n: number) => number): UiElement {
  // The portrait sits in the ring, empty for a hidden or missing hero.
  const face = px(28)
  const portrait: UiElement = {
    type: 'panel',
    props: {
      style: {
        width: face,
        height: face,
        verticalAlign: 'center',
        borderRadius: px(3),
        borderWidth: 2,
        borderColor: tones.success,
        background: '#1c201a',
        saturation: row.out ? 0 : 1,
      },
      children:
        row.src === undefined
          ? undefined
          : {
              type: 'image',
              props: {
                src: row.src,
                style: { width: '100%', height: '100%' },
              },
            },
    },
  }
  const name: UiElement = {
    type: 'label',
    props: {
      style: {
        face: 'body',
        weight: 'bold',
        fontSize: px(16),
        color: row.color,
        verticalAlign: 'center',
        margin: [0, 0, 0, px(8)],
        maxWidth: px(200),
      },
      children: row.name,
    },
  }
  const icon: UiElement = {
    type: 'image',
    props: {
      src: voiceIcon,
      style: {
        width: px(18),
        height: px(16),
        verticalAlign: 'center',
        margin: [0, 0, 0, px(8)],
        tint: tones.success,
      },
    },
  }
  return {
    type: 'panel',
    key: `speaker-${row.slot}`,
    props: {
      speaking: row.player,
      style: {
        flow: 'right',
        horizontalAlign: 'left',
        background: '#10130d9e',
        borderRadius: px(3),
        padding: [px(4), px(12), px(4), px(4)],
        margin: [0, 0, px(4), 0],
      },
      children: [portrait, name, icon],
    },
  }
}
