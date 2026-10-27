// The HUD kit: components that draw a mod's HUD in the stock HUD's look, from
// the tokens in theme.ts. Text sets VALVEPulp capitals over a soft shadow,
// plates and pills hold text over a bright sky, and pips count as the stock
// round counter does. The roster lists the players down the top right corner.
// Actions are the game's menu buttons, with its hover and press sounds, and a
// meter fills toward its value.
//
//   show(player, <TopStrip heading="Round 2"><Score points={[1, 0]} /></TopStrip>)

import { clip } from './format.js'
import { heroes } from './heroes.js'
import {
  emptyPip,
  motion,
  fpsReadoutBottom,
  objectiveBarBottom,
  pillEdge,
  pipRings,
  type PipShape,
  pipShapes,
  plate,
  shades,
  sizes,
  sounds,
  teamBarBottom,
  type TextRole,
  type Tone,
  tones,
  types,
} from './theme.js'
import type { Player } from './host.gen.js'
import type { Child, Style, UiElement } from './ui.js'

/**
 * element builds a panel or label for the kit's components; a later style
 * overrides an earlier one.
 */
function element(
  type: 'panel' | 'label',
  style: Style,
  children?: Child,
  key?: string,
): UiElement {
  return { type, props: { style, children }, key }
}

/** TextProps set one label in a type role and tone. */
export interface TextProps {
  role: TextRole
  /** tone colors the text; a label is muted and the rest are text. */
  tone?: Tone
  /**
   * plated marks text on a plate or pill, which takes no shadow: on the dark
   * plate a shadow only smears the glyphs.
   */
  plated?: boolean
  style?: Style
  children?: Child
}

/** Text is one label in a type role, over the soft shadow unless plated. */
export function Text(props: TextProps): UiElement {
  const type = types[props.role]
  return element(
    'label',
    {
      face: type.face,
      fontSize: type.size,
      uppercase: type.uppercase,
      shadow: !props.plated,
      color: tones[props.tone ?? (props.role === 'label' ? 'muted' : 'text')],
      horizontalAlign: 'center',
      textAlign: 'center',
      ...props.style,
    },
    props.children,
  )
}

/** BoxProps lay out a plate's or pill's children. */
export interface BoxProps {
  flow?: 'down' | 'right'
  style?: Style
  children?: Child
}

/** Plate is the dark plate under HUD text, readable over a bright sky. */
export function Plate(props: BoxProps): UiElement {
  return element(
    'panel',
    {
      flow: props.flow ?? 'down',
      horizontalAlign: 'center',
      background: plate.color,
      borderRadius: plate.radius,
      padding: plate.padding,
      ...props.style,
    },
    props.children,
  )
}

/**
 * Pill is the plate rounded into a capsule with a faint
 * parchment edge, flowing right: the counter under a top strip's heading.
 */
export function Pill(props: BoxProps): UiElement {
  return element(
    'panel',
    {
      flow: props.flow ?? 'right',
      horizontalAlign: 'center',
      background: plate.color,
      borderRadius: 50,
      borderWidth: 2,
      borderColor: pillEdge,
      padding: [3, 6],
      ...props.style,
    },
    props.children,
  )
}

/** PipsProps show a count out of a fixed total. */
export interface PipsProps {
  filled: number
  total: number
  tone: Tone
  /** shape is a round point, as the stock round counter, or a narrow tick. */
  shape?: PipShape
  /** mirror fills from the right, for the far side of a counter. */
  mirror?: boolean
  style?: Style
}

/** Pips shows a count as filled and empty pips, as the stock round counter. */
export function Pips(props: PipsProps): UiElement {
  const shape = pipShapes[props.shape ?? 'round']
  const lit = (index: number) =>
    props.mirror ? index >= props.total - props.filled : index < props.filled
  return element(
    'panel',
    { flow: 'right', verticalAlign: 'center', ...props.style },
    Array.from({ length: props.total }, (_, index) =>
      element(
        'panel',
        {
          width: shape.width,
          height: shape.height,
          borderRadius: shape.radius,
          margin: [0, 2],
          background: lit(index) ? tones[props.tone] : emptyPip,
          borderWidth: 2,
          borderColor: lit(index) ? pipRings.filled : pipRings.empty,
        },
        undefined,
        `pip-${index}`,
      ),
    ),
  )
}

/** TagProps name a status or a result in small capitals. */
export interface TagProps {
  tone: Tone
  /** fill outlines a status read in a list, or fills a result. */
  fill?: 'outline' | 'filled'
  style?: Style
  children?: Child
}

/** Tag is small heading capitals in a 1px border of its tone. */
export function Tag(props: TagProps): UiElement {
  const filled = props.fill === 'filled'
  return element(
    'label',
    {
      face: 'block',
      fontSize: 12,
      letterSpacing: 1,
      uppercase: true,
      color: filled ? tones.onAccent : tones[props.tone],
      background: filled ? tones[props.tone] : undefined,
      borderWidth: 1,
      borderColor: tones[props.tone],
      borderRadius: sizes.radius,
      padding: [4, 8],
      verticalAlign: 'center',
      ...props.style,
    },
    props.children,
  )
}

/** BannerProps announce a moment: a title and a detail under it. */
export interface BannerProps {
  title: string
  detail?: string
  tone?: Tone
  style?: Style
}

/** Banner is a centered title in the heading face with a muted detail. */
export function Banner(props: BannerProps): UiElement {
  return element(
    'panel',
    {
      flow: 'down',
      horizontalAlign: 'center',
      margin: [20, 0, 0, 0],
      ...props.style,
    },
    [
      Text({ role: 'title', tone: props.tone, children: props.title }),
      props.detail &&
        Text({ role: 'caption', tone: 'muted', children: props.detail }),
    ],
  )
}

/** TopStripProps fill the top block of an arena HUD. */
export interface TopStripProps {
  /** heading names the mode or round and the lane, over the counter. */
  heading?: string
  /** line is a short line under the counter, such as a clock or the news. */
  line?: string
  lineTone?: Tone
  /**
   * keepTeamBar seats the strip under the stock team bar, which keeps its
   * clock, heroes and player slots; unset, it takes the bar's place.
   */
  keepTeamBar?: boolean
  /** children are the counter's content, such as Score and Pips. */
  children?: Child
}

/**
 * TopStrip is the top block of an arena HUD: a heading in spaced capitals, the
 * counter pill, and a line under it. Under the kept team bar, the heading and
 * pill fit above the stock objective bar and the line goes below that bar.
 */
export function TopStrip(props: TopStripProps): UiElement {
  // The heading sits over the counter pill, and the line reads under both.
  const block = [
    props.heading &&
      Text({
        role: 'label',
        style: { letterSpacing: 1 },
        children: props.heading,
      }),
    Pill({ style: { margin: [4, 0, 0, 0] }, children: props.children }),
  ]
  const line =
    props.line &&
    Text({
      role: 'label',
      tone: props.lineTone ?? 'text',
      style: { margin: [4, 0, 0, 0], fontSize: 18, letterSpacing: 1 },
      children: props.line,
    })

  // A column centers its parts from top down. In the bar's place, one column
  // holds them all; under the kept bar, the line has its own below the
  // objective bar.
  const column = (top: number, children: Child) =>
    element(
      'panel',
      { flow: 'down', horizontalAlign: 'center', margin: [top, 0, 0, 0] },
      children,
    )
  if (!props.keepTeamBar) return column(14, [...block, line])
  return element('panel', { width: 'fill', height: 'fill' }, [
    column(teamBarBottom, block),
    line && column(objectiveBarBottom, line),
  ])
}

/** ScoreProps are each side's points, the near side first. */
export interface ScoreProps {
  points: readonly [number, number]
  /** tones color each side; unset, the near side is the accent. */
  tones?: readonly [Tone, Tone]
}

/**
 * Score is a counter's points, each side in its tone, in the title face. It
 * sits in a pill, so its text is plated.
 */
export function Score(props: ScoreProps): UiElement {
  const [near, far] = props.tones ?? ['accent', 'text']
  const side = (points: number, tone: Tone) =>
    Text({
      role: 'title',
      tone,
      plated: true,
      style: { margin: [0, 10], horizontalAlign: undefined },
      children: `${points}`,
    })
  return element('panel', { flow: 'right', verticalAlign: 'center' }, [
    side(props.points[0], near),
    Text({
      role: 'title',
      tone: 'muted',
      plated: true,
      style: { horizontalAlign: undefined },
      children: ':',
    }),
    side(props.points[1], far),
  ])
}

/** RosterRow is one player as the roster shows them. */
export interface RosterRow {
  name: string
  /** self marks the viewing player's own row. */
  self?: boolean
  /** hero is the player's hero as Pawn.hero gives it, or 0 without one. */
  hero: number
  /** activity is the line under the name, such as "Free roam". */
  activity: string
}

/** rosterRows is the most players the roster shows, above the minimap. */
export const rosterRows = 6

/** rosterName is the most characters a roster name shows. */
const rosterName = 24

/** portraitSize is the portrait square's side, its 1px edge included. */
const portraitSize = 44

/** portraits maps each hero's id to its small portrait. */
const portraits = new Map(heroes.map((hero) => [hero.id, hero.portrait]))

/** rosterKey is what the roster shows, to redraw only when it changes. */
export function rosterKey(rows: readonly RosterRow[]): string {
  return rows
    .slice(0, rosterRows)
    .map(
      (row) =>
        `${row.self ? '*' : ''}${row.name}\t${row.hero}\t${row.activity}`,
    )
    .join('\n')
}

/** RosterProps list the players and where the roster starts. */
export interface RosterProps {
  rows: readonly RosterRow[]
  /** top is the roster's top margin, under the FPS readout unless set. */
  top?: number
}

/**
 * Roster stacks up to rosterRows players down the top right corner, each
 * one's hero portrait at the edge with their name and a line, such as what
 * they are doing or their score, to its left. It shows nothing without rows.
 * A mode that hides the stock scoreboard's heroes shows who else is here with
 * it.
 */
export function Roster(props: RosterProps): UiElement | null {
  if (props.rows.length === 0) return null
  return element(
    'panel',
    {
      width: 'fit',
      height: 'fit',
      flow: 'down',
      horizontalAlign: 'right',
      verticalAlign: 'top',
      margin: [props.top ?? fpsReadoutBottom, 24, 0, 0],
    },
    props.rows.slice(0, rosterRows).map(rosterRow),
  )
}

/**
 * rosterRow is one player: name and activity, then the portrait, whose edge
 * reads in the accent on the viewing player's own row.
 */
function rosterRow(row: RosterRow, index: number): UiElement {
  // The name and its line align right, against the portrait.
  const right = { horizontalAlign: 'right', textAlign: 'right' } as const
  const text = element(
    'panel',
    { flow: 'down', verticalAlign: 'center', margin: [0, 12, 0, 0] },
    [
      Text({
        role: 'label',
        tone: 'text',
        style: right,
        children: clip(row.name.toUpperCase(), rosterName),
      }),
      Text({
        role: 'caption',
        tone: 'muted',
        style: { ...right, margin: [1, 0, 0, 0] },
        children: row.activity,
      }),
    ],
  )

  // The portrait sits in a 1px edge on a raised square, empty without a hero.
  const src = portraits.get(row.hero)
  const portrait = element(
    'panel',
    {
      width: portraitSize,
      height: portraitSize,
      padding: 1,
      verticalAlign: 'center',
      background: row.self ? tones.accent : shades.dim,
    },
    element(
      'panel',
      { width: '100%', height: '100%', background: shades.raised },
      src && {
        type: 'image',
        props: { src, style: { width: '100%', height: '100%' } },
      },
    ),
  )
  return element(
    'panel',
    { flow: 'right', horizontalAlign: 'right', margin: [0, 0, 8, 0] },
    [text, portrait],
    `row-${index}`,
  )
}

/** ActionProps make a button in the game's menu look. */
export interface ActionProps {
  /**
   * kind is 'primary' for the one action a screen leads with, 'secondary' for
   * the rest, and 'quiet' for an action that should not draw the eye.
   */
  kind?: 'primary' | 'secondary' | 'quiet'
  /** disabled dims the action and takes no presses. */
  disabled?: boolean
  onPress?: (player: Player) => void
  style?: Style
  children?: Child
}

/** actionLooks are each kind of action's fill, edge and text, idle and hovered. */
const actionLooks = {
  primary: {
    idle: { background: tones.text },
    hover: { background: shades.white },
    text: shades.ink,
  },
  secondary: {
    idle: {
      background: shades.raised,
      borderWidth: 1,
      borderColor: shades.faint,
    },
    hover: { background: shades.lit, borderColor: tones.text },
    text: tones.text,
  },
  quiet: {
    idle: { background: shades.clear },
    hover: { background: shades.raised },
    text: shades.quiet,
  },
} as const

/**
 * Action is a button in the game's menu look: capitals on a fill that lights
 * as the pointer enters, with the main menu's hover and press sounds.
 */
export function Action(props: ActionProps): UiElement {
  const look = actionLooks[props.kind ?? 'secondary']
  const label = element(
    'label',
    {
      face: 'block',
      fontSize: 16,
      uppercase: true,
      letterSpacing: 1,
      color: look.text,
      horizontalAlign: 'center',
      verticalAlign: 'center',
      noWrap: true,
    },
    props.children,
  )
  return {
    type: 'button',
    props: {
      style: {
        height: sizes.control,
        padding: [0, 14],
        borderRadius: sizes.radius,
        transition: motion.hover,
        ...look.idle,
        ...(props.disabled ? { opacity: 0.4 } : {}),
        ...props.style,
      },
      hover: props.disabled ? undefined : look.hover,
      hoverSound: props.disabled ? undefined : sounds.hover,
      sound: sounds.press,
      disabled: props.disabled,
      onPress: props.onPress,
      children: label,
    },
  }
}

/** MeterProps show how full something is. */
export interface MeterProps {
  /** value is how full the meter is, from 0 to 1. */
  value: number
  tone?: Tone
  /** width and height size the meter, in pixels; 160 by 8 unless set. */
  width?: number
  height?: number
  style?: Style
}

/**
 * Meter is a bar that fills from the left toward its value, easing to each new
 * value: a charge, a capture or a timer.
 */
export function Meter(props: MeterProps): UiElement {
  const height = props.height ?? 8
  const value = Math.min(1, Math.max(0, props.value))
  return element(
    'panel',
    {
      width: props.width ?? 160,
      height,
      background: shades.raised,
      borderRadius: height / 2,
      overflow: 'clip',
      ...props.style,
    },
    // The fill scales rather than resizes, so it eases without layout work.
    element('panel', {
      width: '100%',
      height: '100%',
      background: tones[props.tone ?? 'accent'],
      scale: [value, 1],
      origin: [0, 50],
      transition: motion.meter,
    }),
  )
}
