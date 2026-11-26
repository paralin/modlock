// The HUD kit: components that draw a mod's HUD in the stock HUD's look, from
// the tokens in theme.ts. Text sets VALVEPulp capitals over a soft shadow,
// plates and pills hold text over a bright sky, and pips count as the stock
// round counter does. The roster lists the players down the right side, in
// groups such as teams, shrinking its rows as players join so all of them fit.
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
  minimapTop,
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
import type { Child, Clock, Style, UiElement } from './ui.js'

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

/**
 * arriving eases a component in from slightly smaller and fades it out as it
 * goes, over a quarter second.
 */
function arriving(component: UiElement, from: Style): UiElement {
  return {
    ...component,
    props: {
      ...component.props,
      style: {
        transition: { seconds: 0.25, easing: 'out' },
        ...component.props.style,
      },
      enter: { opacity: 0, ...from },
      exit: { opacity: 0 },
    },
  }
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
  /** clock counts the text's time in the player's game in place of children. */
  clock?: Clock
  style?: Style
  children?: Child
}

/** Text is one label in a type role, over the soft shadow unless plated. */
export function Text(props: TextProps): UiElement {
  const type = types[props.role]
  const text = element(
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
  return props.clock
    ? { ...text, props: { ...text.props, clock: props.clock } }
    : text
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

/**
 * Banner is a centered title in the heading face with a muted detail. It
 * grows in as it appears and fades as it goes.
 */
export function Banner(props: BannerProps): UiElement {
  const banner = element(
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
  return arriving(banner, { scale: 0.92 })
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
  /**
   * key names the player across draws, such as their slot, so their row
   * keeps its panels as others join, leave or change places; the row's index
   * in its group otherwise.
   */
  key?: string | number
  name: string
  /** self marks the viewing player's own row. */
  self?: boolean
  /**
   * hero is the player's hero as Pawn.hero gives it, or 0 to show none, as
   * for a player without one or whose hero this viewer may not know.
   */
  hero: number
  /**
   * activity is the line under the name, such as "Free roam". It shows only
   * while the roster has room for it, so a row's state must not depend on it
   * alone; out does that.
   */
  activity?: string
  /**
   * steamId is the player's Steam account. Its avatar badges the corner of
   * the hero's portrait, or fills the square when no hero shows.
   */
  steamId?: bigint
  /**
   * out marks a player who is out of play, such as eliminated or
   * spectating: the row dims and its picture loses its color.
   */
  out?: boolean
  /**
   * onPress makes the row a button the viewer presses while they have a
   * cursor, such as to open a card about that player.
   */
  onPress?: (player: Player) => void
  /** selected edges the portrait in the accent, as for the row a menu shows. */
  selected?: boolean
  /**
   * menu draws beside the row: left of the roster, its top level with the
   * row's, such as the menu a press of the row opens. One menu shows at a
   * time, the first row's that has one, and only while that row has room.
   * rosterKey leaves the menu out, so a caller keys its contents itself.
   */
  menu?: UiElement
}

/** RosterGroup is one category of players under a heading, such as a team. */
export interface RosterGroup {
  /** key names the group across draws, so it keeps its panels. */
  key: string
  /** title heads the group, such as "Seekers"; without one the rows stand alone. */
  title?: string
  /** tone colors the title, such as the team's color; muted by default. */
  tone?: Tone
  /** note follows the title in muted text, such as a count. */
  note?: string
  rows: readonly RosterRow[]
}

/**
 * RosterDensity is one size the roster draws its rows at: the portrait's
 * side, the distance from one row's top to the next, the name's size, whether
 * the activity line shows, the avatar badge's side (0 for none), and the most
 * columns of rows it may lay side by side.
 */
interface RosterDensity {
  readonly portrait: number
  readonly pitch: number
  readonly nameSize: number
  readonly activity: boolean
  readonly badge: number
  readonly columns: number
}

/**
 * rosterDensities are the roster's sizes, largest first. The roster takes the
 * first that fits its height in the fewest columns, so each row gets smaller
 * as players join: the full row with its activity line, then a compact row
 * with the name alone, then a dense row that may run in up to three columns.
 */
const rosterDensities: readonly RosterDensity[] = [
  {
    portrait: 44,
    pitch: 52,
    nameSize: 15,
    activity: true,
    badge: 18,
    columns: 1,
  },
  {
    portrait: 32,
    pitch: 38,
    nameSize: 15,
    activity: false,
    badge: 14,
    columns: 1,
  },
  {
    portrait: 24,
    pitch: 27,
    nameSize: 13,
    activity: false,
    badge: 0,
    columns: 3,
  },
]

/** rosterName is the most characters a roster name shows. */
const rosterName = 24

/**
 * rosterCell is a dense row's width, its name included, so the rows of each
 * column line up whatever the names.
 */
const rosterCell = 150

/** rosterColumnGap is the space a dense cell keeps clear on its left, between its name and the column before it. */
const rosterColumnGap = 12

/** rosterHeading is a group heading's height, its gap to the rows included. */
const rosterHeading = 24

/** rosterGroupGap is the space above every group but the first. */
const rosterGroupGap = 10

/** rosterMenuGap is the space between a row's menu and the roster. */
const rosterMenuGap = 8

/** rosterGap is the space between the roster's end and what it must clear. */
const rosterGap = 12

/** portraits maps each hero's id to its small portrait. */
const portraits = new Map(heroes.map((hero) => [hero.id, hero.portrait]))

/** rosterKey is what the roster shows, to redraw only when it changes. */
export function rosterKey(groups: readonly RosterGroup[]): string {
  return groups
    .map(
      (group) =>
        `${group.key}\t${group.title ?? ''}\t${group.tone ?? ''}\t${group.note ?? ''}\n${group.rows
          .map(
            (row) =>
              `${row.key ?? ''}\t${row.self ? '*' : ''}${row.selected ? '+' : ''}${row.out ? '-' : ''}${row.onPress ? '>' : ''}${row.name}\t${row.hero}\t${row.activity ?? ''}\t${row.steamId ?? ''}`,
          )
          .join('\n')}`,
    )
    .join('\n\n')
}

/** RosterProps list the players and the band of the screen the roster fits in. */
export interface RosterProps {
  /** rows are one group of players without a heading. */
  rows?: readonly RosterRow[]
  /** groups are categories of players, each under its heading, in order. */
  groups?: readonly RosterGroup[]
  /** top is the roster's top margin, under the FPS readout unless set. */
  top?: number
  /**
   * bottom is how far down the screen the roster must end, in pixels on a
   * 1080 high HUD: above the minimap unless set. A mode with its own panel
   * on the right passes that panel's top.
   */
  bottom?: number
}

/** RosterLayout is the density and columns a roster draws at, and how many lines each group keeps. */
interface RosterLayout {
  density: RosterDensity
  columns: number
  /** lines is each group's lines of rows; a group cut short ends in a count of the rest. */
  lines: readonly number[]
  /** height is the roster's drawn height, headings and gaps included. */
  height: number
}

/** linesOf is how many lines a group's rows take in columns. */
function linesOf(rows: number, columns: number): number {
  return Math.ceil(rows / columns)
}

/**
 * rosterLayout picks the largest density, in the fewest columns, whose rows
 * fit the height. When even the smallest does not fit, each group keeps a
 * line and the rest of the lines go round the groups one at a time, so the
 * roster always ends above bottom and no group crowds out another.
 */
function rosterLayout(
  groups: readonly RosterGroup[],
  height: number,
): RosterLayout {
  // The headings and the gaps between groups take their height first.
  const chrome = groups.reduce(
    (sum, group, index) =>
      sum +
      (group.title === undefined ? 0 : rosterHeading) +
      (index > 0 ? rosterGroupGap : 0),
    0,
  )

  // The first density and column count whose lines fit wins.
  for (const density of rosterDensities) {
    for (let columns = 1; columns <= density.columns; columns++) {
      const lines = groups.map((group) => linesOf(group.rows.length, columns))
      const total = lines.reduce((sum, count) => sum + count, 0)
      if (chrome + total * density.pitch <= height) {
        return {
          density,
          columns,
          lines,
          height: chrome + total * density.pitch,
        }
      }
    }
  }

  // Nothing fits, so the smallest density in its most columns gives each
  // group one line and shares out the lines left.
  const density = rosterDensities[rosterDensities.length - 1]!
  const columns = density.columns
  const wanted = groups.map((group) => linesOf(group.rows.length, columns))
  const lines = groups.map(() => 1)
  let spare =
    Math.max(groups.length, Math.floor((height - chrome) / density.pitch)) -
    groups.length

  // The spare lines go round the groups that want more, one at a time.
  while (spare > 0 && lines.some((count, at) => count < wanted[at]!)) {
    for (let at = 0; at < lines.length && spare > 0; at++) {
      if (lines[at]! < wanted[at]!) {
        lines[at] = lines[at]! + 1
        spare -= 1
      }
    }
  }
  const total = lines.reduce((sum, count) => sum + count, 0)
  return { density, columns, lines, height: chrome + total * density.pitch }
}

/** RosterPlan is what a roster draws: its nonempty groups, where, and how. */
interface RosterPlan {
  groups: readonly RosterGroup[]
  top: number
  layout: RosterLayout
}

/** rosterPlan lays out props, or is null when no group has a row. */
function rosterPlan(props: RosterProps): RosterPlan | null {
  // Rows alone are one group without a heading; empty groups draw nothing.
  const groups = (
    props.groups ?? [{ key: 'players', rows: props.rows ?? [] }]
  ).filter((group) => group.rows.length > 0)
  if (groups.length === 0) return null

  // The band between top and bottom sets the density.
  const top = props.top ?? fpsReadoutBottom
  const bottom = (props.bottom ?? minimapTop) - rosterGap
  return { groups, top, layout: rosterLayout(groups, bottom - top) }
}

/**
 * rosterBottom is how far down the screen the roster of props ends, so a
 * panel can sit under it; it is top when the roster draws nothing.
 */
export function rosterBottom(props: RosterProps): number {
  const plan = rosterPlan(props)
  return plan ? plan.top + plan.layout.height : (props.top ?? fpsReadoutBottom)
}

/**
 * Roster lists the players down the right side of the screen, under the FPS
 * readout and above the minimap, so a mode that hides the stock top bar still
 * shows who is here. Each row is the player's hero portrait at the edge with
 * their name and a line, such as what they are doing, to its left. Groups put
 * the players under headings, such as one per team. Rows shrink as players
 * join, then run in columns, so every player fits between top and bottom; if
 * they still cannot, each group shows as many as fit and a count of the rest.
 * It shows nothing without rows.
 */
export function Roster(props: RosterProps): UiElement | null {
  // Without a row there is nothing to draw.
  const plan = rosterPlan(props)
  if (!plan) return null

  // The groups stack down the right edge, and a row's menu hangs to their
  // left, level with its row. The column keeps its place whether a menu
  // shows, so opening one keeps every row's panels.
  const { groups, top, layout } = plan
  const menu = rosterMenu(groups, layout)
  return element(
    'panel',
    {
      width: 'fit',
      height: 'fit',
      flow: 'right',
      horizontalAlign: 'right',
      verticalAlign: 'top',
      margin: [top, 24, 0, 0],
    },
    [
      menu &&
        element(
          'panel',
          { margin: [menu.offset, rosterMenuGap, 0, 0] },
          menu.element,
          'menu',
        ),
      element(
        'panel',
        { flow: 'down', horizontalAlign: 'right' },
        groups.map((group, index) =>
          rosterGroup(group, index, layout, layout.lines[index]!),
        ),
        'column',
      ),
    ],
  )
}

/**
 * rosterMenu finds the first shown row with a menu, and how far its top sits
 * below the roster's: the gaps and headings of the groups above it, then
 * its line's place in its group. A row the roster had no room for shows no
 * menu.
 */
function rosterMenu(
  groups: readonly RosterGroup[],
  layout: RosterLayout,
): { element: UiElement; offset: number } | undefined {
  let offset = 0
  for (const [index, group] of groups.entries()) {
    offset +=
      (index > 0 ? rosterGroupGap : 0) +
      (group.title === undefined ? 0 : rosterHeading)
    const lines = layout.lines[index]!
    const room = lines * layout.columns
    const shown = group.rows.length > room ? room - 1 : group.rows.length
    const at = group.rows.findIndex((row) => row.menu !== undefined)
    if (at >= 0 && at < shown) {
      return {
        element: group.rows[at]!.menu!,
        offset: offset + Math.floor(at / layout.columns) * layout.density.pitch,
      }
    }
    offset += lines * layout.density.pitch
  }
  return undefined
}

/**
 * rosterGroup is one group: its heading, then its rows, laid in lines of
 * the layout's columns. One column puts the rows straight in the group, so
 * the common case keeps each row's panels as its neighbors come and go.
 */
function rosterGroup(
  group: RosterGroup,
  index: number,
  layout: RosterLayout,
  lines: number,
): UiElement {
  // A group with less room than rows ends in a count of the rest.
  const { density, columns } = layout
  const room = lines * columns
  const cut = group.rows.length > room
  const shown = cut ? group.rows.slice(0, room - 1) : group.rows
  const cells: UiElement[] = shown.map((row, at) =>
    rosterRow(row, at, density, columns > 1),
  )
  if (cut) cells.push(rosterRest(group.rows.length - shown.length, density))

  // In columns, the cells go in lines of fixed width cells, right aligned.
  const body =
    columns === 1
      ? cells
      : Array.from({ length: lines }, (_, line) =>
          element(
            'panel',
            { flow: 'right', horizontalAlign: 'right' },
            cells.slice(line * columns, (line + 1) * columns),
            `line-${line}`,
          ),
        )
  const heading =
    group.title === undefined
      ? false
      : element(
          'panel',
          {
            flow: 'right',
            horizontalAlign: 'right',
            height: rosterHeading,
          },
          [
            Text({
              role: 'label',
              tone: group.tone ?? 'muted',
              style: { letterSpacing: 1, horizontalAlign: undefined },
              children: group.title,
            }),
            group.note !== undefined &&
              Text({
                role: 'label',
                tone: 'muted',
                style: { margin: [0, 0, 0, 8], horizontalAlign: undefined },
                children: group.note,
              }),
          ],
        )
  return element(
    'panel',
    {
      flow: 'down',
      horizontalAlign: 'right',
      margin: [index > 0 ? rosterGroupGap : 0, 0, 0, 0],
    },
    [heading, ...body],
    `group-${group.key}`,
  )
}

/**
 * rosterRow is one player: name and activity, then the portrait, whose edge
 * reads in the accent on the viewing player's own row. A row in columns has
 * a fixed width, so the columns line up.
 */
function rosterRow(
  row: RosterRow,
  index: number,
  density: RosterDensity,
  cell: boolean,
): UiElement {
  // The name and its line align right, against the portrait.
  const right = { horizontalAlign: 'right', textAlign: 'right' } as const
  const nameWidth = cell
    ? rosterCell - density.portrait - 8 - rosterColumnGap
    : undefined
  const text = element(
    'panel',
    { flow: 'down', verticalAlign: 'center', margin: [0, 8, 0, 0] },
    [
      Text({
        role: 'label',
        tone: row.out ? 'muted' : 'text',
        style: {
          ...right,
          fontSize: density.nameSize,
          ...(nameWidth === undefined
            ? {}
            : { width: nameWidth, noWrap: true, textOverflow: 'ellipsis' }),
        },
        children: clip(row.name.toUpperCase(), rosterName),
      }),
      density.activity &&
        row.activity !== undefined &&
        row.activity !== '' &&
        Text({
          role: 'caption',
          tone: 'muted',
          style: { ...right, margin: [1, 0, 0, 0] },
          children: row.activity,
        }),
    ],
  )

  // The portrait sits in a 1px edge on a raised square: the hero with the
  // player's avatar on its lower left corner, the avatar alone without a
  // hero, or empty without either. An out player's picture loses its color.
  const src = portraits.get(row.hero)
  const picture =
    src !== undefined
      ? [
          portraitImage(src),
          density.badge > 0 &&
            row.steamId !== undefined &&
            avatarBadge(row.steamId, density.badge),
        ]
      : row.steamId !== undefined
        ? [avatarFill(row.steamId)]
        : []
  const portrait = element(
    'panel',
    {
      width: density.portrait,
      height: density.portrait,
      padding: 1,
      verticalAlign: 'center',
      background: row.self || row.selected ? tones.accent : shades.dim,
    },
    element(
      'panel',
      {
        width: '100%',
        height: '100%',
        background: shades.raised,
        saturation: row.out ? 0 : 1,
      },
      picture,
    ),
  )

  // A player who joins slides in from the edge. A pressable row is a button
  // whose ground lifts under the pointer.
  const style: Style = {
    flow: 'right',
    horizontalAlign: 'right',
    height: density.pitch,
    opacity: row.out ? 0.55 : 1,
    ...(cell ? { width: rosterCell } : {}),
  }
  const key = `row-${row.key ?? index}`
  const line: UiElement = row.onPress
    ? {
        type: 'button',
        props: {
          style: {
            ...style,
            borderRadius: sizes.radius,
            transition: motion.hover,
          },
          hover: { background: shades.raised },
          hoverSound: sounds.hover,
          sound: sounds.press,
          onPress: row.onPress,
          children: [text, portrait],
        },
        key,
      }
    : element('panel', style, [text, portrait], key)
  return arriving(line, { translate: [16, 0] })
}

/** rosterRest is the count of a group's players the roster had no room for. */
function rosterRest(count: number, density: RosterDensity): UiElement {
  return element(
    'panel',
    { height: density.pitch, width: rosterCell, horizontalAlign: 'right' },
    Text({
      role: 'label',
      tone: 'muted',
      style: {
        fontSize: density.nameSize,
        horizontalAlign: 'right',
        verticalAlign: 'center',
        textAlign: 'right',
      },
      children: `+${count} more`,
    }),
    'rest',
  )
}

/**
 * portraitImage is a hero's portrait filling its square. It is built in a
 * function with a declared result: a literal element with a nested style,
 * held as a UiElement | false through &&, is not representable in a scriptc
 * build's Child union.
 */
function portraitImage(src: string): UiElement {
  return {
    type: 'image',
    props: { src, style: { width: '100%', height: '100%' } },
  }
}

/** avatarFill is a player's avatar filling the portrait's square. */
function avatarFill(steamId: bigint): UiElement {
  return {
    type: 'avatar',
    props: { steamId, style: { width: '100%', height: '100%' } },
  }
}

/** avatarBadge is a player's avatar on the lower left corner of a portrait. */
function avatarBadge(steamId: bigint, side: number): UiElement {
  return {
    type: 'avatar',
    props: {
      steamId,
      style: {
        width: side,
        height: side,
        horizontalAlign: 'left',
        verticalAlign: 'bottom',
        borderRadius: side / 2,
        borderWidth: 1,
        borderColor: shades.dim,
      },
    },
  }
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

/** ActionLook is one kind of action's fill, edge and text, idle and hovered. */
interface ActionLook {
  idle: Style
  hover: Style
  text: string
}

/** actionLooks are each kind of action's look. */
const actionLooks: Record<NonNullable<ActionProps['kind']>, ActionLook> = {
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
}

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
