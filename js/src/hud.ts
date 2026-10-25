// The HUD kit: components that draw a mod's HUD in the stock HUD's look, from
// the tokens in theme.ts. Text sets VALVEPulp capitals over a soft shadow,
// plates and pills hold text over a bright sky, and pips count as the stock
// round counter does.
//
//   show(player, <TopStrip heading="Round 2"><Score points={[1, 0]} /></TopStrip>)

import {
  emptyPip,
  objectiveBarBottom,
  pillEdge,
  pipRings,
  type PipShape,
  pipShapes,
  plate,
  sizes,
  teamBarBottom,
  type TextRole,
  type Tone,
  tones,
  types,
} from './theme.js'
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
