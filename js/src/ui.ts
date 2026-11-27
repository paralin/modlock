// A mod shows each player an interface written with JSX:
//
//   import { show } from 'modlock'
//
//   show(player, (
//     <panel style={{ flow: 'down', horizontalAlign: 'center', margin: [80, 0, 0, 0] }}>
//       <label style={{ fontSize: 32, bold: true }}>Round {round}</label>
//       <button onPress={(player) => ready(player)}>Ready</button>
//     </panel>
//   ))
//
// The player's game draws each element with a stock panel, so no mod code
// runs there. show sends only the elements that changed since the last call
// for that player, so a mod may call it whenever its state changes, or every
// frame.
//
// Every element takes a style and a hover style, which the game draws while
// the pointer is over it and animates to when the style has a transition. A
// tooltip and a hover sound show and play on the same hover; a button plays
// its sound when pressed. A hero element shows one of the game's pictures of
// a hero, such as <hero hero={player.hero} look="small" />.
//
// An element's enter style is where its panel starts when it first shows,
// and its exit style where it goes as it leaves; its transition eases each
// way, so <panel style={{ opacity: 1, transition: { seconds: 0.2 } }}
// enter={{ opacity: 0 }}> fades in. A cycle swings an element to another style
// and back while it shows, as a pulse or a blink.
//
// Inputs take a value from the player and call onChange with the player and
// the value the host checked: <entry> a line of text, <slider> a number in its
// range, <toggle> on or off, <dropdown> the index of an option, and <range>
// every handle's value once the player lets one go. The value the mod draws
// is the truth; the player's edit shows until the mod's next value for that
// input differs from its last.
//
// A list scrolls rows far more numerous than an interface's nodes may be,
// such as a leaderboard of every player. Each row is drawn from the row
// template, whose text, images and variables name the row's fields as
// "{field}"; the host holds the rows and the player's game builds panels only
// for the rows in view:
//
//   <list style={{ height: 400 }} rowHeight={32}
//     row={<label>{'{name}: {score}'}</label>}
//     rows={players.map((p) => ({ key: p.id, rank: -p.score,
//       fields: { name: p.name, score: String(p.score) } }))} />
//
// Each draw compares every row with those the player holds and sends only the
// rows added, changed and removed.
//
// A label with a token shows the game's own text in the player's language,
// such as <label token="#Citadel_HeroName_Haze" />, and an avatar shows a
// player's Steam picture: <avatar steamId={info.steamId} />.
//
// A layout element loads a layout from the mod's content addon, such as
// <layout src="shop.xml" data={stock} />. Its script runs in the player's
// game, so only a host that has reviewed the mod's content loads it; elsewhere
// it draws nothing. The script includes the Panorama library
// scripts/modlock/layout.js and reads data with Modlock.data. It presses
// back with Modlock.press('buy', { item: 'boots' }), which runs the element's
// onAction with the player who pressed, the action and the payload:
// <layout src="shop.xml" onAction={(player, action, payload) => ...} />.
//
// route sends a player a named JSON value that every layout of the mod reads
// with Modlock.route, wherever it sits in the tree, such as
// route(player, 'scores', { red: 3, blue: 1 }). The value stays until the
// next call; undefined removes it.
//
// toast shows a player a short notice at the lower left, above the game's
// health and stats, which fades after a few seconds; a mod answers a command
// or refuses one with it.
//
// hideHud hides parts of the game's own HUD from a player, such as their
// health bar or the notice for each boon, until the mod lists them no more,
// hides the player's interface, or stops. hideHudForAll hides parts from
// every player, those who join later included, on top of each player's own. Modlock gives them back on its own
// when the player leaves, by the menu, to the hideout or with a server that
// goes away, so a mod has nothing to clean up.
//
// uiSound plays one of the game's sound events, such as
// 'UI.ItemDraft.Start', in one player's game alone, once, with or without a
// hero on the map; { seconds } cuts it after that long. A player who leaves
// hears what was playing stop.
//
// sheet adds a style sheet from the mod's content addon to every interface
// the mod shows, such as sheet('hud.css') for content/styles/hud.css, and an
// element's class names the sheet's classes:
//
//   <label class={leading ? 'Score Leading' : 'Score'}>{score}</label>
//
// A sheet styles only the mod's own panels, a later sheet wins over an
// earlier one, and an element's style wins over both. A class change runs the
// sheet's transitions. Sheets load where layouts do; elsewhere the elements
// draw with their own styles.

import { itemId } from './item-id.js'
import {
  type AbilityRow as WireAbilityRow,
  Align,
  type Binding as BindingNode,
  type Change,
  type Clip as WireClip,
  type Clock as WireClock,
  ClockFormat,
  type Cue,
  type Cycle as WireCycle,
  Decoration,
  Easing,
  Face,
  Flow,
  Frame,
  GameAction as WireGameAction,
  GradientShape,
  HeroLook,
  HudPart as WireHudPart,
  Kind,
  MenuMode,
  Overflow as WireOverflow,
  Scaling as WireScaling,
  TextOverflow,
  type Edges as WireEdges,
  type Gradient as WireGradient,
  type Length as WireLength,
  type ListChange,
  type Node,
  type Row,
  type Shadow as WireShadow,
  type Style as WireStyle,
  type Transform as WireTransform,
  type Transition as WireTransition,
  UiRequest,
  Weight,
} from '../../src/wasm/ui/ui.pb.js'
import { chatLines, expireChat, forgetChat, hearChat } from './chat.js'
import { forgetSpeakers, speakers, speakersChanged } from './speakers.js'
import { heroes } from './heroes.js'
import {
  type ActionPhase,
  type ChatEvent,
  type GameAction,
  type Overlay,
  type Player,
  readField,
  type DocumentFailure,
  type UiDocumentEvent,
  type UiActionEvent,
  type UiOverlayEvent,
  type UiPlacementEvent,
  type Bounds,
  type Wheel,
  type RangeEvent,
} from './host.gen.js'
import { call } from './host.js'
import { connection, players } from './players.js'

/**
 * Length is a size: pixels, a percentage of the parent, 'fit' to size to the
 * children, or 'fill' to share the space left in the parent's flow.
 */
export type Length = number | `${number}%` | 'fit' | 'fill'

/**
 * Edges are distances in pixels: one for every side, [vertical, horizontal],
 * or [top, right, bottom, left]. They are an array rather than a union of
 * tuples, which scriptc does not compile.
 */
export type Edges = number | readonly number[]

/**
 * Overflow decides what happens to children that do not fit: 'squish' shrinks
 * them, 'clip' cuts them off, 'scroll' lets the player scroll to them, and
 * 'show' draws them outside the element.
 */
export type Overflow = 'squish' | 'clip' | 'scroll' | 'show'

/**
 * Gradient blends colors across an element in place of its background. The
 * colors run down by default; at places each color from 0 to 1, spaced evenly
 * when unset.
 */
export interface Gradient {
  shape?: 'down' | 'right' | 'up' | 'left' | 'radial'
  colors: readonly string[]
  at?: readonly number[]
}

/**
 * Shadow is a soft shadow, offset by x and y and softened by blur, in pixels.
 * Spread grows a box's shadow in pixels, or strengthens a text shadow, where 0
 * draws it at full strength; inset draws a box's shadow inside its edge.
 */
export interface Shadow {
  x?: number
  y?: number
  blur?: number
  spread?: number
  color: string
  inset?: boolean
}

/**
 * Transition animates each change of an element's style, and the move to its
 * hover style and back, over seconds after delay. Easing is 'out' unless set.
 * Curve shapes the change in place of easing as a cubic Bezier curve's two
 * control points, [x1, y1, x2, y2], as CSS's cubic-bezier: each x is a share
 * of the time from 0 to 1, and a y outside 0 to 1 overshoots, such as
 * [0.75, 1.5, 0.95, 1.4] for a pop that springs past its end.
 */
export interface Transition {
  seconds: number
  delay?: number
  easing?: 'out' | 'in' | 'inOut' | 'linear'
  curve?: readonly number[]
}

/**
 * Clip draws only part of an element and its children. Rect is a box,
 * [top, right, bottom, left], each side a percent of the element's size from
 * its top or left edge, so [0, 50, 100, 0] is its left half. Without rect, the
 * clip is a slice of a circle about center, [x, y] percents of the element's
 * size and its middle unless set, through sweep degrees from start, as the
 * game draws a cooldown ring. A transition does not ease a clip; draw it
 * again to fill a ring over time.
 */
export interface Clip {
  rect?: readonly number[]
  center?: readonly number[]
  start?: number
  sweep?: number
}

/**
 * Clock counts a label's time in the player's game, so the mod draws it once
 * and not every second. It starts at seconds as the player first sees it and
 * counts down to zero, or up when up is set, unless paused. Format writes
 * 'minutes' as "4:05", 'seconds' as "245" or 'tenths' as "9.4". A change to
 * any of its fields starts the count again from seconds; change run to
 * restart the same clock, such as at each round.
 */
export interface Clock {
  seconds: number
  up?: boolean
  paused?: boolean
  format?: 'minutes' | 'seconds' | 'tenths'
  run?: number
}

/**
 * Style lays out and paints an element. Colors are '#rrggbb' or '#rrggbbaa'.
 * Options that take one or two numbers are an array rather than a union of
 * tuples, which scriptc does not compile.
 */
export interface Style {
  width?: Length
  height?: Length
  /** minWidth, maxWidth, minHeight and maxHeight bound the element's size. */
  minWidth?: Length
  maxWidth?: Length
  minHeight?: Length
  maxHeight?: Length
  /**
   * flow places the children in a column or a row, or in columns or rows that
   * wrap where one is full; unset stacks them.
   */
  flow?: 'down' | 'right' | 'downWrap' | 'rightWrap'
  /**
   * x and y place the element at an offset from its parent's top left corner,
   * in pixels or a percent of the parent, outside the parent's flow.
   */
  x?: number | `${number}%`
  y?: number | `${number}%`
  /** zIndex draws the element above its siblings with a lower index. */
  zIndex?: number
  /** overflow decides what children that do not fit do, across and down. */
  overflow?: Overflow
  /** overflowX and overflowY set overflow for one direction. */
  overflowX?: Overflow
  overflowY?: Overflow
  horizontalAlign?: 'left' | 'center' | 'right'
  verticalAlign?: 'top' | 'center' | 'bottom'
  margin?: Edges
  padding?: Edges
  background?: string
  color?: string
  fontSize?: number
  bold?: boolean
  textAlign?: 'left' | 'center' | 'right'
  borderRadius?: number
  /** opacity fades the element and its children, from 0 to 1. */
  opacity?: number
  /**
   * face is one of the game's typefaces: 'body' its text, 'block' the
   * capitals of its clock and titles, 'serif' its menus.
   */
  face?: 'body' | 'block' | 'serif'
  /** uppercase sets the text in capitals. */
  uppercase?: boolean
  /** letterSpacing widens the space between letters, in pixels. */
  letterSpacing?: number
  /** shadow draws a soft drop shadow under the text, to read over the world. */
  shadow?: boolean
  /** textShadow draws this shadow under the text in place of shadow's. */
  textShadow?: Shadow
  /**
   * borderWidth and borderColor draw a line around the element's edge; it
   * shows only when both are set.
   */
  borderWidth?: number
  borderColor?: string
  /** gradient fills the element in place of its background. */
  gradient?: Gradient
  /** boxShadow draws a shadow around or inside the element's box. */
  boxShadow?: Shadow
  /** tint washes the element and its children with a color, recoloring a white icon. */
  tint?: string
  /** saturation, brightness and contrast scale the colors; 1 leaves them. */
  saturation?: number
  brightness?: number
  contrast?: number
  /** blur blurs the element and its children by a radius in pixels. */
  blur?: number
  italic?: boolean
  /** weight sets the text's weight in place of bold. */
  weight?:
    | 'thin'
    | 'light'
    | 'normal'
    | 'medium'
    | 'semiBold'
    | 'bold'
    | 'black'
  /** lineHeight is the height of each line of text, in pixels. */
  lineHeight?: number
  /** noWrap keeps the text on one line. */
  noWrap?: boolean
  /**
   * textOverflow ends text that does not fit with an 'ellipsis', 'shrink's it
   * until it fits, or 'clip's it at the edge.
   */
  textOverflow?: 'ellipsis' | 'shrink' | 'clip'
  /** decoration draws a line under or through the text. */
  decoration?: 'underline' | 'strike'
  /** translate moves the element as drawn by [x, y] pixels. */
  translate?: readonly number[]
  /** scale scales the element as drawn: one factor, or [x, y]. */
  scale?: number | readonly number[]
  /** rotate turns the element clockwise, in degrees. */
  rotate?: number
  /**
   * turn turns the element clockwise, in degrees, before its other
   * transforms. A transition eases the angle itself, so a turn from 0 to 720
   * spins twice, where rotate eases the shortest way round.
   */
  turn?: number
  /** clip draws only part of the element and its children. */
  clip?: Clip
  /**
   * origin is the point the element scales and turns about, as [x, y]
   * percents of its size; unset is its center.
   */
  origin?: readonly number[]
  /**
   * transition animates each change of the style: seconds, or a Transition.
   * Animate transform, opacity and colors; size and placement animate too, but
   * move the element's neighbors each frame.
   */
  transition?: number | Transition
}

/**
 * HudPart is a part of the game's own HUD: 'levelUp' the notice for each boon,
 * ability point and power increase, 'ownHealth' the player's health bar with
 * its shields and regeneration, 'topBar' the bar of team scores, portraits
 * and clock across the top, 'ammo' the clip ring, count and ammo warnings at
 * the crosshair, 'souls' the souls and level readout, 'minimap' the minimap
 * with its hints and district name, 'abilities' the ability and active item
 * bar, 'status' the game's mark in the top-left corner, 'stats' the weapon,
 * spirit and vitality stats at the lower left, 'heroPicker' the screen a
 * player without a hero picks one on, 'brokerCountdown' the countdown in the
 * Broker's window, 'minimapPlayers' every player's icon on the minimap, the
 * player's own included, 'inventory' the purchased items beside the stats,
 * 'crosshair' the crosshair with its clip ring, ammo count and stamina pips,
 * 'castFailed' the notice of an ability the player could not cast,
 * 'eventIndicators' the icons and numbers the game floats at each event that
 * affects the player, 'statusIndicators' only those that name a crowd control
 * status, such as a stun, and 'all' the whole HUD but the mods' own interfaces, the game's tooltips
 * and its chat, for a screen that takes the player's whole view. Whatever a
 * part does not name stays.
 */
export type HudPart =
  | 'levelUp'
  | 'ownHealth'
  | 'topBar'
  | 'ammo'
  | 'souls'
  | 'minimap'
  | 'abilities'
  | 'status'
  | 'stats'
  | 'heroPicker'
  | 'brokerCountdown'
  | 'minimapPlayers'
  | 'inventory'
  | 'crosshair'
  | 'castFailed'
  | 'eventIndicators'
  | 'statusIndicators'
  | 'all'

/** Child is anything an element may hold. Text becomes a label. */
export type Child =
  | UiElement
  | string
  | number
  | boolean
  | null
  | undefined
  | readonly Child[]

/** Component returns the elements that stand for its props. */
export type Component<P> = (props: P) => Child

/**
 * UiElement is one element of an interface, as JSX builds it. A component's
 * element is a fragment holding what the component returned.
 */
export interface UiElement {
  readonly type: string
  readonly props: Props
  readonly key?: string
}

/** MenuProps describes a surface whose visibility and focus stay local. */
export interface MenuProps extends PanelProps {
  mode?: 'manual' | 'hold' | 'toggle'
  openLabel?: string
  openRequest?: number
  priority?: number
}

/**
 * DocumentInputProps describes a blank draft submitted only with its button.
 * onSubmit receives each document that arrives whole, and onFailure each
 * submission that ended without one, so the mod can tell the player. A
 * failure is undefined when a newer host gives a reason this library does not
 * name.
 */
export interface DocumentInputProps extends ElementProps {
  placeholder?: string
  submitLabel?: string
  disabled?: boolean
  onSubmit: (player: Player, text: string) => void
  onFailure?: (player: Player, failure?: DocumentFailure) => void
}

/** DocumentOutputProps shows up to 262,144 UTF-8 bytes the player can select. */
export interface DocumentOutputProps extends ElementProps {
  text: string
}

/**
 * ChangeHandler is the handler of any input: it takes the value the control
 * holds, whose type each element's props narrow. The method form makes the
 * parameter bivariant, so a handler of a text, a number or a flag fits, and
 * the host checks the value against the handler's own type when it calls; a
 * parameter of type never would refuse every value in a scriptc build.
 */
type ChangeHandler = {
  bivarianceHack(player: Player, value: unknown): void
}['bivarianceHack']

/**
 * Props are an element's props: every intrinsic element's, by name, so the
 * encoder reads them as typed fields. A component's props pass through to it.
 */
export interface Props {
  readonly class?: string
  readonly style?: Style
  readonly hover?: Style
  readonly tooltip?: string
  readonly item?: string
  readonly ability?: AbilityTip
  readonly hoverSound?: string
  readonly measure?: string
  readonly children?: Child
  readonly src?: string
  readonly scaling?: Scaling
  readonly hero?: number
  readonly look?: HeroLookName
  readonly data?: unknown
  readonly sound?: string
  readonly disabled?: boolean
  readonly focusOrder?: number
  readonly initialFocus?: boolean
  readonly submits?: boolean
  readonly onPress?: (player: Player) => void
  readonly onAction?: LayoutPressHandler
  readonly bindings?: Readonly<Record<string, Binding>>
  readonly enter?: Style
  readonly exit?: Style
  readonly cycle?: Cycle
  readonly hideInChat?: boolean
  readonly speaking?: Player
  readonly token?: string
  readonly variables?: Readonly<Record<string, string | number>>
  readonly clock?: Clock
  readonly steamId?: bigint
  readonly value?: string | number
  readonly placeholder?: string
  readonly maxLength?: number
  readonly min?: number
  readonly max?: number
  readonly step?: number
  readonly on?: boolean
  readonly options?: readonly string[]
  readonly selected?: number | string
  readonly focusStyle?: Style
  readonly selectedStyle?: Style
  readonly handles?: readonly RangeHandle[]
  readonly rowHeight?: number
  readonly row?: Child
  readonly rows?: readonly ListRow[]
  readonly onChange?: ChangeHandler
  readonly onHold?: (player: Player, handle: string, phase: ActionPhase) => void
  readonly onWheel?: (player: Player, wheel: Wheel) => void
  readonly mode?: 'manual' | 'hold' | 'toggle'
  readonly openLabel?: string
  readonly openRequest?: number
  readonly priority?: number
  readonly submitLabel?: string
  readonly text?: string
  readonly onSubmit?: (player: Player, text: string) => void
  readonly onFailure?: (player: Player, failure?: DocumentFailure) => void
}

/**
 * ElementProps are the props every drawn element takes. hover is the style
 * drawn over style while the pointer is over the element; tooltip is the text
 * the game's tooltip shows then, item names an item by class name, such as
 * 'upgrade_ability_power_shard', whose shop card shows instead, ability names
 * an ability whose window shows instead of either, and hoverSound is the
 * game's sound event played as the pointer enters, such as
 * 'UI.MainMenu.Hover'. An element with any of them takes the pointer, so the
 * game behind it does not.
 */
export interface ElementProps {
  /** class names the element's classes in the mod's sheets, separated by spaces. */
  class?: string
  style?: Style
  hover?: Style
  tooltip?: string
  item?: string
  ability?: AbilityTip
  hoverSound?: string
  /** measure names the element in the placement reports of followPlacement. */
  measure?: string
  /** enter is the style the element's panel starts from when it first shows. */
  enter?: Style
  /** exit is the style the element's panel eases to as it leaves. */
  exit?: Style
  /** cycle swings the element to another style and back while it shows. */
  cycle?: Cycle
  /**
   * hideInChat hides the element while the player has the game's chat box
   * open, which then shows the chat history itself.
   */
  hideInChat?: boolean
  /**
   * speaking shows the element only while the player hears this player on
   * voice chat, and for a moment after. The player's game shows and hides
   * it, so voices starting and stopping draw nothing again.
   */
  speaking?: Player
}

/**
 * AbilityTip is the game's own window for an ability, in the player's
 * language: name is the ability's class name, such as 'ability_bebop_hook',
 * and player is whose hero the window reads its numbers from, a hero that
 * stands on the map, such as a bot that plays it.
 */
export interface AbilityTip {
  readonly name: string
  readonly player: Player
}

/**
 * Cycle swings an element from its style to style and back, each way taking
 * seconds, count times or, without a count, while it shows.
 */
export interface Cycle {
  style: Style
  seconds: number
  count?: number
}

/** PanelProps lay out a panel, which groups its children. */
export interface PanelProps extends ElementProps {
  children?: Child
}

/**
 * LabelProps show text: the label's text children, joined, or the game's text
 * for token in the player's language, with each {s:name} place in it filled
 * from variables. A clock's time stands in for the text, or fills the token's
 * {s:clock} place.
 */
export interface LabelProps extends ElementProps {
  children?: Child
  token?: string
  variables?: Readonly<Record<string, string | number>>
  clock?: Clock
}

/** AvatarProps show the Steam picture of the player with steamId. */
export interface AvatarProps extends ElementProps {
  steamId: bigint
}

/**
 * InputProps are the props every input takes. A disabled input takes no
 * changes.
 */
export interface InputProps extends ElementProps, FocusProps {
  disabled?: boolean
}

/**
 * FocusProps place a button or input in the keyboard's reach: focusOrder is
 * its place in the game's keyboard and controller navigation, lowest first,
 * and initialFocus gives it the keyboard when its menu opens.
 */
export interface FocusProps {
  focusOrder?: number
  initialFocus?: boolean
}

/**
 * EntryProps make a box the player types a line into: value is its text,
 * placeholder the faint text it shows empty, and maxLength its limit in
 * characters, 100 unless set, and at most 240 bytes of text.
 */
export interface EntryProps extends InputProps {
  value?: string
  placeholder?: string
  maxLength?: number
  onChange?: (player: Player, text: string) => void
}

/**
 * SliderProps make a bar the player drags to a number from min to max, 0 to 1
 * unless set, stopping at each step when step is set.
 */
export interface SliderProps extends InputProps {
  value?: number
  min?: number
  max?: number
  step?: number
  onChange?: (player: Player, value: number) => void
  /** onWheel receives one wheel step and its pointer anchor while enabled. */
  onWheel?: (player: Player, wheel: Wheel) => void
}

/** RangeHandle is one handle of a range: its name and the value it holds. */
export interface RangeHandle {
  readonly name: string
  readonly value: number
}

/**
 * RangeProps make a track with several handles the player drags, such as a
 * minimum, a favored value and a maximum, from min to max, 0 to 1 unless set,
 * stopping at each step when step is set. The handles keep their order: one
 * stops at its neighbors. While the player holds a handle the game moves it,
 * and onChange receives every handle's value, in order, when they let go.
 * Left and right step the active handle from the keyboard or a controller,
 * confirm makes the next handle active, and each step reaches onChange at
 * once and onHold as 'released'.
 * onHold hears each hold: 'pressed' as the player takes a handle, then
 * 'released', or 'cancelled' when a stock overlay or the menu's closing ended
 * it and put the handles back.
 */
export interface RangeProps extends InputProps {
  handles: readonly RangeHandle[]
  min?: number
  max?: number
  step?: number
  onChange?: (player: Player, values: number[]) => void
  onHold?: (player: Player, handle: string, phase: ActionPhase) => void
}

/**
 * ListRow is one row of a list: its key, 1 to 64 bytes and distinct within
 * the list, the rank and sort text that order the rows, least rank first, and
 * the fields its template's "{field}" names show, at most sixteen, each named
 * by letters, digits and '_'.
 */
export interface ListRow {
  readonly key: string
  readonly rank?: number
  readonly sort?: string
  readonly fields?: Readonly<Record<string, string>>
}

/**
 * ListProps make a list that scrolls its rows, drawing only those in view.
 * rowHeight is each row's height in pixels, from 1 to 1000, and row the
 * template each row is drawn from: a panel, label or image holding at most
 * fifteen more, with no buttons or inputs. The host takes at most 20,000 rows,
 * and 2 MiB of rows across a player's interface.
 *
 * The player focuses a row with the arrow keys or the controller, and chooses
 * it with Enter, the controller's confirm or a click, which calls onChange
 * with the row's key. The focus stays on its row as rows come and go. The
 * mod keeps the chosen row and draws it back as selected, which shows until
 * its row leaves the list. focusStyle and selectedStyle paint the focused and
 * the selected row over the row's own style.
 */
export interface ListProps extends ElementProps {
  rowHeight: number
  row: Child
  rows: readonly ListRow[]
  selected?: string
  focusStyle?: Style
  selectedStyle?: Style
  onChange?: (player: Player, key: string) => void
}

/** ToggleProps make a box the player checks, labeled by its text children. */
export interface ToggleProps extends InputProps {
  children?: Child
  on?: boolean
  onChange?: (player: Player, on: boolean) => void
}

/** DropdownProps let the player pick one of options; selected is its index. */
export interface DropdownProps extends InputProps {
  options: readonly string[]
  selected?: number
  onChange?: (player: Player, index: number) => void
}

/**
 * Scaling fits a picture into its element's box: 'fit' shows it whole, 'cover'
 * fills the box and crops, 'stretch' fills the box and stretches, 'fitWidth'
 * and 'fitHeight' fit one side, and 'none' draws it at its own size.
 */
export type Scaling =
  | 'fit'
  | 'cover'
  | 'stretch'
  | 'fitWidth'
  | 'fitHeight'
  | 'none'

/**
 * ImageProps show an image from the game's content, such as
 * 'file://{images}/hud/icon.png', or from the mod's own content with {addon}
 * in place of its addon's name, such as 'file://{images}/{addon}/mark.png'.
 */
export interface ImageProps extends ElementProps {
  src: string
  scaling?: Scaling
}

/**
 * HeroLookName is one of the game's pictures of a hero: 'small' the square
 * portrait of the kill feed, 'card' the hero picker's tall card, 'icon' the
 * round icon, 'vertical' the hero grid's narrow portrait, 'topBar' the top
 * bar's portrait and 'minimap' the minimap icon.
 */
export type HeroLookName =
  | 'small'
  | 'card'
  | 'icon'
  | 'vertical'
  | 'topBar'
  | 'minimap'

/** HeroProps show a picture of hero, by the game's hero id. */
export interface HeroProps extends ElementProps {
  hero: number
  look?: HeroLookName
  scaling?: Scaling
}

/**
 * ButtonProps make a button, which holds its children and calls onPress
 * with the player who pressed it. Players press buttons while they have a
 * cursor. sound is the game's sound event the press plays, such as
 * 'UI.MainMenu.Activate', and a disabled button takes no presses.
 */
export interface ButtonProps extends ElementProps, FocusProps {
  children?: Child
  sound?: string
  disabled?: boolean
  /** submits makes Enter in an entry of the button's menu press it. */
  submits?: boolean
  onPress?: (player: Player) => void
}

/**
 * LayoutPressHandler hears a press a layout's script sent: the player the
 * server says sent it, its action and its JSON payload, undefined for none.
 */
export type LayoutPressHandler = (
  player: Player,
  action: string,
  payload: unknown,
) => void

/**
 * LayoutProps load a layout from the mod's content addon: src is its path
 * there, such as 'shop.xml'. The layout keeps the fonts, art and localized
 * strings its author drew, and bindings set parts of its panels by their ids,
 * at most 64, so a layout needs no script. data is the JSON value a layout's
 * own script reads with Modlock.data, and onAction hears each press its
 * script sends with Modlock.press. A layout with bindings takes presses only
 * from the bindings with an onPress.
 */
export interface LayoutProps {
  class?: string
  style?: Style
  src: string
  data?: unknown
  bindings?: Readonly<Record<string, Binding>>
  onAction?: LayoutPressHandler
  /** measure names the layout in the placement reports of followPlacement. */
  measure?: string
}

/**
 * Binding sets parts of one panel of a layout and leaves the rest as the
 * layout's author drew it. An unset field keeps the author's value, and one
 * the mod stops setting returns to it. A panel the layout lacks leaves the
 * binding undrawn and the rest drawn.
 */
export interface Binding {
  /** text replaces a label's words. */
  text?: string
  /**
   * token replaces a label's words with the game's text for it in the
   * player's language, each {s:name} place filled from variables.
   */
  token?: string
  variables?: Readonly<Record<string, string | number>>
  /** visible shows or hides the panel. */
  visible?: boolean
  /** class adds classes from the layout's sheets, separated by spaces. */
  class?: string
  /** style sets inline style; a property it drops returns to the author's. */
  style?: Style
  /**
   * src sets an image's picture from the game's content. The game cannot
   * read a picture back, so the last one set stays until the layout loads
   * again.
   */
  src?: string
  /** onPress runs when the player presses the panel. */
  onPress?: (player: Player) => void
}

/** JSX types the elements a mod's JSX may use. */
export declare namespace JSX {
  type Element = UiElement
  // The compiler applies IntrinsicAttributes only to components, so each
  // element takes a key as well.
  interface IntrinsicElements {
    menu: MenuProps & IntrinsicAttributes
    menuClose: PanelProps & IntrinsicAttributes
    documentInput: DocumentInputProps & IntrinsicAttributes
    documentOutput: DocumentOutputProps & IntrinsicAttributes
    panel: PanelProps & IntrinsicAttributes
    label: LabelProps & IntrinsicAttributes
    image: ImageProps & IntrinsicAttributes
    hero: HeroProps & IntrinsicAttributes
    button: ButtonProps & IntrinsicAttributes
    layout: LayoutProps & IntrinsicAttributes
    avatar: AvatarProps & IntrinsicAttributes
    entry: EntryProps & IntrinsicAttributes
    slider: SliderProps & IntrinsicAttributes
    toggle: ToggleProps & IntrinsicAttributes
    dropdown: DropdownProps & IntrinsicAttributes
    range: RangeProps & IntrinsicAttributes
    list: ListProps & IntrinsicAttributes
  }
  interface IntrinsicAttributes {
    key?: string | number
  }
  interface ElementChildrenAttribute {
    children: unknown
  }
}

/**
 * jsx builds one element, rendering a component in place; the TypeScript
 * compiler calls it for JSX.
 */
export function jsx(
  type: string | Component<Props>,
  props: Props,
  key?: string | number,
): UiElement {
  const named = key === undefined ? undefined : String(key)
  if (typeof type === 'function') {
    return { type: fragment, props: { children: type(props) }, key: named }
  }
  return { type, props, key: named }
}

/** fragment is the type of a component's element. */
const fragment = 'fragment'

/** jsxs builds an element with several children, as jsx does. */
export const jsxs = jsx

/** Fragment groups children without a panel of its own. */
export function Fragment(props: { children?: Child }): Child {
  return props.children
}

/**
 * show replaces the interface the mod shows player with root. A player who
 * left and a mod that restarted show nothing until the next call. An element
 * shown again as the same object at the same place reuses what it laid out
 * last time without laying it out again, so a mod may keep a subtree whose
 * inputs have not changed and pass it again; an element's props must not
 * change after it is shown.
 */
export function show(player: Player, root: Child): boolean {
  views.set(player.slot, { ...view(player), root })
  return draw(player, ['view'])
}

/**
 * hideHud hides parts of the game's own HUD from player until the next call;
 * an empty list shows them all again.
 */
export function hideHud(player: Player, parts: readonly HudPart[]): boolean {
  views.set(player.slot, { ...view(player), hud: parts })
  return draw(player, [])
}

/**
 * hideHudForAll hides parts of the game's own HUD from every player, now and
 * as each joins, until the next call; an empty list shows them all again. A
 * player's own hideHud list adds to it.
 */
export function hideHudForAll(parts: readonly HudPart[]): void {
  everyoneHud = parts
  drawAll([])
}

/**
 * freeCursor shows player's mouse cursor without Tab held while free is true,
 * so they can click the mod's panels, such as a pick screen; false gives the
 * mouse back to aiming. Each call with true shows the cursor again, even when
 * the player put it away with the game's own keys; a screen that changes
 * without a call leaves their choice be. The game does not stop the player's
 * shots, camera or movement while the cursor is up: a screen that must not
 * fire a weapon calls player.blockInput for the buttons it takes.
 */
export function freeCursor(player: Player, free: boolean): boolean {
  const current = view(player)
  views.set(player.slot, {
    ...current,
    cursor: free,
    asks: free ? current.asks + 1 : current.asks,
  })
  return draw(player, [])
}

/** AbilityBarHandler takes a click on the ability in slot, 0 to 3. */
export type AbilityBarHandler = (player: Player, slot: number) => void

/**
 * abilityBarPresses calls onPress with the slot, 0 to 3, each time player
 * clicks the icon or upgrade pips of an ability on the game's own ability bar,
 * as the game does when it trains one with Tab held; without onPress the
 * clicks stop reaching the mod. The game answers a click from the hero's own
 * abilities, and sends nothing for an ability replaceAbility gave, so a mod
 * that replaces abilities answers here, with trainAbility. A click the game
 * answers itself, with upgrade_ability, reaches the mod both ways. Clicks
 * reach the mod only while its screen holds the player's HUD: a player who
 * left, or whose session ended, sends none.
 */
export function abilityBarPresses(
  player: Player,
  onPress?: AbilityBarHandler,
): boolean {
  views.set(player.slot, {
    ...view(player),
    bar: onPress ? { handler: onPress } : undefined,
  })
  return draw(player, [])
}

/** ActionHandler takes one phase of a game action player used. */
export type ActionHandler = (player: Player, phase: ActionPhase) => void

/**
 * followAction calls onPhase with each phase of action, a game action such as
 * 'scoreboard', each time player uses it, on whichever key they bound it to:
 * 'pressed' once, then 'released' or 'cancelled' once. The game still does
 * what the action does. A press made while a stock overlay, such as chat, the
 * shop or the escape menu, has the player's input reports nothing, and one
 * held as such an overlay opens is cancelled. Without onPhase the mod stops
 * following. Actions reach the mod only while its screen holds the player's
 * HUD.
 */
export function followAction(
  player: Player,
  action: GameAction,
  onPhase?: ActionHandler,
): boolean {
  // Replace the view, so a draw sees the new list of followed actions.
  const current = view(player)
  const actions = new Map(current.actions)
  if (onPhase) {
    actions.set(action, { handler: onPhase })
  } else {
    actions.delete(action)
  }
  views.set(player.slot, { ...current, actions })
  return draw(player, [])
}

/** acted runs the handler of the game action a player used. */
export function acted(event: UiActionEvent): void {
  const { player, action, phase } = event
  if (action === undefined || phase === undefined) {
    return
  }
  views.get(player.slot)?.actions.get(action)?.handler(player, phase)
}

/** OverlayHandler takes the stock overlay that now holds player's input. */
export type OverlayHandler = (player: Player, overlay: Overlay) => void

/**
 * followOverlay calls onChange with the stock overlay, such as 'chat', 'shop'
 * or 'escapeMenu', that holds player's input: first the current one, then
 * each time a different one takes it, or 'none' as the game gets it back.
 * When several are open it names the first of chat, the shop, the build
 * editor, the item draft, a popup and the escape menu. Without onChange the
 * mod stops following.
 */
export function followOverlay(
  player: Player,
  onChange?: OverlayHandler,
): boolean {
  // Replace the view, so a draw sees whether the root follows overlays.
  const current = view(player)
  views.set(player.slot, {
    ...current,
    overlay: onChange && { handler: onChange },
  })
  return draw(player, [])
}

/** overlaid runs the handler of the mod that follows a player's overlays. */
export function overlaid(event: UiOverlayEvent): void {
  const { player, overlay } = event
  views.get(player.slot)?.overlay?.handler(player, overlay ?? 'none')
}

/**
 * Placement is where a player's screen places the interface, in layout
 * units from the screen's top left, where the screen is 1080 units tall:
 * the screen's size, the HUD's frame, and the box of each element whose
 * measure names it.
 */
export interface Placement {
  /** screen is the whole screen, at 0, 0. */
  readonly screen: Bounds
  /** hud is the HUD's frame, the middle 16:9 of a wider screen that keeps it there. */
  readonly hud: Bounds
  /** measured holds the box of each measured element, by its name. */
  readonly measured: ReadonlyMap<string, Bounds>
}

/** PlacementHandler takes where player's screen places the interface. */
export type PlacementHandler = (player: Player, placement: Placement) => void

/**
 * followPlacement calls onPlace with where player's screen places the
 * interface: the whole placement first, then again each time any part of it
 * moves, such as when the player changes resolution or an element's layout
 * settles. Elements that share a measure report one box. Without onPlace the
 * mod stops following.
 */
export function followPlacement(
  player: Player,
  onPlace?: PlacementHandler,
): boolean {
  // Replace the view, so a draw sees whether the root follows placement.
  const current = view(player)
  views.set(player.slot, {
    ...current,
    placement: onPlace && { handler: onPlace },
  })
  return draw(player, [])
}

/**
 * inHudFrame draws player's interface inside the HUD's frame instead of the
 * whole screen. The frame is the screen itself, except on a screen wider
 * than 16:9 where the player keeps the HUD in its middle 16:9, so an
 * interface aligned to the frame's edges lines up with the game's HUD.
 */
export function inHudFrame(player: Player, on: boolean): boolean {
  const current = view(player)
  views.set(player.slot, { ...current, hudFrame: on })
  return draw(player, [])
}

/** placed runs the handler of the mod that follows a player's placement. */
export function placed(event: UiPlacementEvent): void {
  const { player, screen, hud, measured } = event
  views.get(player.slot)?.placement?.handler(player, {
    screen,
    hud,
    measured: new Map(measured.map(({ name, bounds }) => [name, bounds])),
  })
}

/** ShownAbility is the ability abilityDisplay shows in one slot. */
export interface ShownAbility {
  /** name is the ability's class name, whose window the game shows over it. */
  readonly name: string
  /** image is its picture, a path in the game's content as an image's src. */
  readonly image: string
  /** upgrades holds its bought tiers as Ability.upgrades: bit 0 unlocks it. */
  readonly upgrades?: number
}

/** AbilityRow is the four abilities, in slot order, shown for one player. */
export interface AbilityRow {
  readonly player: Player
  /** abilities holds slots 0 to 3; an undefined slot shows the game's own. */
  readonly abilities: readonly (ShownAbility | undefined)[]
}

/**
 * abilityDisplay shows viewer each row's abilities in place of the ones the
 * game shows for that player's hero: on the player's row of the game's Tab
 * screen, and for viewer's own row on viewer's ability bar too, in the game's
 * frame, ring and pips, with the game's ability window over each. The game
 * draws these from the hero's standard abilities, so one replaceAbility gave
 * shows as the ability it replaced. Without rows the game's own come back,
 * as they do when viewer leaves or the session ends. Rows match the Tab
 * screen's by the player's name and else by the hero's, which a bot's row
 * shows, so each row's player should have a hero.
 */
export function abilityDisplay(
  viewer: Player,
  rows: readonly AbilityRow[] = [],
): boolean {
  views.set(viewer.slot, { ...view(viewer), display: rows.slice(0, 64) })
  return draw(viewer, [])
}

/**
 * displayRows puts rows on the wire for viewer, with each player's name and
 * hero as the Tab screen shows them now.
 */
function displayRows(
  viewer: Player,
  rows: readonly AbilityRow[],
): WireAbilityRow[] {
  // Read the names once, and each hero's class from its id.
  if (rows.length === 0) return []
  const names = new Map(players().map((c) => [c.player.slot, c.name]))
  return rows.map((row) => {
    const hero = row.player.pawn()?.hero
    return {
      player: row.player.slot,
      // A Steam name is at most 32 characters, within the host's 128 bytes.
      name: (names.get(row.player.slot) ?? '').slice(0, 32),
      hero: heroes.find((each) => each.id === hero)?.name ?? '',
      own: row.player.slot === viewer.slot,
      abilities: row.abilities.slice(0, 4).map((shown) => ({
        ability: shown?.name ? itemId(shown.name) : 0,
        image: shown?.image ?? '',
        upgrades: shown?.upgrades ?? 0,
      })),
    }
  })
}

/** abilityBarNode matches the node of a click on the game's ability bar. */
const abilityBarNode = /^ability:([0-3])$/

/**
 * cursorFree reports whether player's mouse cursor is up now, whether the mod
 * freed it with freeCursor or the player did with the game's own keys. It is
 * undefined while the player has no hero to read.
 */
export function cursorFree(player: Player): boolean | undefined {
  const pawn = player.pawn()
  if (pawn === undefined) return undefined
  return readField(
    pawn.entity,
    'CCitadelPlayer_MovementServices',
    'm_bHasFreeCursor',
    'bool',
  ) as boolean | undefined
}

/**
 * sheet adds the style sheet at path in the mod's content styles directory,
 * such as 'hud.css', to every interface the mod shows, after the sheets added
 * before it. Adding a sheet again changes nothing. A path is letters, digits,
 * '_', '-' and '.' in segments separated by '/', ending in '.css'; another
 * adds nothing and returns false. The build turns an import of a sheet, such
 * as import './content/styles/hud.css', into this call.
 */
export function sheet(path: string): boolean {
  // Refuse a path the renderer cannot load, and keep a sheet added before.
  if (
    !path.endsWith('.css') ||
    !path.split('/').every((part) => sheetPart.test(part) && part !== '..')
  ) {
    return false
  }
  if (sheets.includes(path)) {
    return true
  }

  // Redraw every interface with the new sheet.
  sheets.push(path)
  for (const current of views.values()) {
    draw(current.player, [])
  }
  return true
}

/** sheetPart matches one segment of a sheet's path. */
const sheetPart = /^[\w.-]+$/

/** sheets lists the mod's style sheets, in the order the mod added them. */
const sheets: string[] = []

/**
 * route sends player value under name, for every layout of the mod to read
 * with Modlock.route, until the next call; undefined removes it. A name is
 * letters, digits, '_', '-' and '.', and the value is anything JSON.stringify
 * takes. A route with another name, or a value JSON cannot hold, sends nothing
 * and returns false.
 */
export function route(player: Player, name: string, value: unknown): boolean {
  // Refuse a name no attribute can carry and a value JSON cannot hold.
  const routes = new Map(view(player).routes)
  const encoded = value === undefined ? undefined : JSON.stringify(value)
  if (!routeName.test(name) || (value !== undefined && encoded === undefined)) {
    return false
  }

  // Store the value, or forget the route, and redraw.
  if (encoded === undefined) {
    routes.delete(name)
  } else {
    routes.set(name, encoded)
  }
  views.set(player.slot, { ...view(player), routes })
  return draw(player, ['routes'])
}

/** routeName matches the name of a route, as the renderer accepts it. */
const routeName = /^[\w.-]+$/

/**
 * hide removes the interface, the routes and the notices the mod shows
 * player. What the mod shows every player, such as hideHudForAll's parts,
 * stays.
 */
export function hide(player: Player): boolean {
  views.delete(player.slot)
  screens.delete(player.slot)
  if (!send(player.slot, { reset: true })) return false
  return everyoneHud.length === 0 || draw(player)
}

/**
 * joined draws what the mod shows every player for a player who joined,
 * such as hideHudForAll's parts.
 */
export function joined(player: Player): void {
  if (everyoneHud.length !== 0) draw(player)
}

/**
 * away forgets the screen of a player whose client disconnected, since the
 * client lost it; returned sends it whole again when they come back. What
 * the mod shows them, such as their notices, stays.
 */
export function away(player: Player): void {
  screens.delete(player.slot)
}

/** returned draws the whole screen of a player who came back. */
export function returned(player: Player): void {
  draw(player)
}

/**
 * left forgets what the mod showed the player who left, so the next person
 * in the slot starts with nothing of theirs, and takes down their chat line.
 */
export function left(player: Player): void {
  views.delete(player.slot)
  screens.delete(player.slot)
  forgetSpeakers(player.slot)
  if (forgetChat(player.slot)) drawAll(['chat'])
}

/** heard shows a chat line to every player who may read it, if the mod shows chat lines. */
export function heard(line: ChatEvent): void {
  if (hearChat(line, now)) drawAll(['chat'])
}

/**
 * toast shows player text in a notice at the lower left for seconds.
 * Notices stack, newest last, slide in, and fade as they expire.
 */
export function toast(player: Player, text: string, seconds = 4): boolean {
  const current = view(player)
  const notice = { id: nextNotice++, text, until: now + seconds }
  views.set(player.slot, {
    ...current,
    notices: [...current.notices, notice].slice(-maxNotices),
  })
  return draw(player, ['notices'])
}

/**
 * uiSound plays the game's sound event sound, such as 'UI.ItemDraft.Start', in
 * player's game alone, once; seconds cuts it after that long, keeping only its
 * start, and delay waits that long before it plays, so one draw can time a
 * run of sounds, such as a wheel's ticks. It plays with the next screen the
 * player reads, and not at all if the player reads it only after the mod's
 * next few sounds, or the mod clears its interface before its delay ends.
 */
export function uiSound(
  player: Player,
  sound: string,
  options: { readonly seconds?: number; readonly delay?: number } = {},
): boolean {
  const current = view(player)
  const cue = {
    number: nextCue++,
    sound,
    seconds: options.seconds ?? 0,
    delay: options.delay ?? 0,
  }
  views.set(player.slot, {
    ...current,
    cues: [...current.cues, cue].slice(-maxCues),
  })
  return draw(player, [])
}

/**
 * press runs the handler of the button the player pressed, or, for a press
 * with an action, the handler of the layout whose script sent it, with the
 * payload's JSON, undefined for none or for text JSON cannot read.
 */
export function press(
  player: Player,
  node: string,
  action = '',
  payload = '',
): void {
  // A press with an action came from a layout's bound panel or its script,
  // not a button.
  if (action !== '') {
    const bound = partOf(player.slot, node)?.presses.get(boundKey(node, action))
    if (bound) {
      bound.handler(player)
      return
    }
    const pressed = partOf(player.slot, node)?.actions.get(node)
    if (pressed) {
      let value: unknown = undefined
      try {
        value = payload === '' ? undefined : (JSON.parse(payload) as unknown)
      } catch {
        value = undefined
      }
      pressed.handler(player, action, value)
    }
    return
  }

  // A click on the game's ability bar names its slot, which no button's id
  // does.
  const bar = abilityBarNode.exec(node)
  if (bar) {
    views.get(player.slot)?.bar?.handler(player, Number(bar[1]))
    return
  }
  const pressed = partOf(player.slot, node)?.presses.get(node)
  if (pressed) {
    pressed.handler(player)
  }
}

/**
 * change runs the handler of the input the player changed, with its value as
 * the host checked it.
 */
export function change(
  player: Player,
  node: string,
  value: string | number | boolean | Wheel | RangeEvent | undefined,
): void {
  if (typeof value === 'object' && value !== null && 'delta' in value) {
    partOf(player.slot, node)?.wheels.get(node)?.handler(player, value)
    return
  }
  const changed = partOf(player.slot, node)?.changes.get(node)
  if (changed) {
    changed.handler(player, value)
  }
}

/**
 * submitted runs the handler of the document input whose submission ended,
 * with the whole document or why it failed.
 */
export function submitted(event: UiDocumentEvent): void {
  const handlers = partOf(event.player.slot, event.node)?.documents.get(
    event.node,
  )
  const result = event.result
  if (!handlers || !result) return
  if ('text' in result) handlers.submit(event.player, result.text)
  else handlers.fail?.(event.player, result.reason)
}

/**
 * tick advances the clock to the frame at seconds and fades expiring notices.
 * It redraws a player only on the frame one of their notices expires, since
 * the renderer animates the fade itself; a frame with no expiry renders
 * nothing.
 */
export function tick(seconds: number): void {
  // Advance the clock, noting a new second, and take down expired lines.
  const second = Math.floor(seconds) !== Math.floor(now)
  now = seconds
  if (expireChat(now)) drawAll(['chat'])

  // Once a second, draw the speaking stack again for each viewer whose rows
  // look different: a player joined, left, picked a hero or changed side.
  if (second) {
    for (const { player, away } of players()) {
      if (!away && speakersChanged(player)) draw(player, ['voice'])
    }
  }
  for (const current of views.values()) {
    if (!current.notices.some((notice) => notice.until <= now)) {
      continue
    }
    current.notices = current.notices.filter((notice) => notice.until > now)
    draw(current.player, ['notices'])
  }
}

/** View is what the mod shows one player: its interface and its notices. */
interface View {
  readonly player: Player
  readonly root: Child
  readonly hud: readonly HudPart[]
  /** cursor is true while the mod frees the player's mouse cursor. */
  readonly cursor: boolean
  /** asks counts the mod's calls that freed the cursor. */
  readonly asks: number
  /** routes maps each route's name to its value as JSON. */
  readonly routes: ReadonlyMap<string, string>
  /** cues are the last sounds played, which the renderer plays once each. */
  readonly cues: readonly Cue[]
  /** bar takes clicks on the game's ability bar, while the mod asks for them. */
  readonly bar: { readonly handler: AbilityBarHandler } | undefined
  /** actions takes each phase of the game actions the mod follows. */
  readonly actions: ReadonlyMap<GameAction, { readonly handler: ActionHandler }>
  /** overlay takes each stock overlay change, while the mod follows them. */
  readonly overlay: { readonly handler: OverlayHandler } | undefined
  /** placement takes each placement report, while the mod follows placement. */
  readonly placement: { readonly handler: PlacementHandler } | undefined
  /** hudFrame is true while the interface is drawn in the HUD's frame. */
  readonly hudFrame: boolean
  /** display is the abilities shown on Tab's rows and the ability bar. */
  readonly display: readonly AbilityRow[]
  notices: readonly Notice[]
}

/** Notice is one toast, shown until the game clock passes until. */
interface Notice {
  readonly id: number
  readonly text: string
  readonly until: number
}

/** views holds what the mod shows each player, by slot. */
const views = new Map<number, View>()

/** everyoneHud is the parts of the game's HUD hidden from every player. */
let everyoneHud: readonly HudPart[] = []

/** now is the game clock at the last frame, in seconds. */
let now = 0

/** nextNotice numbers the next notice, which keys it in the stack. */
let nextNotice = 0

/** nextCue numbers the next sound, so the renderer plays each once. */
let nextCue = 1

/**
 * maxCues is how many sounds the root keeps, so a player whose screens come
 * slower than the mod plays sounds misses none of the last few.
 */
const maxCues = 8

/** maxNotices bounds the stack; a newer notice pushes out the oldest. */
const maxNotices = 4

/** noticeMotion slides a notice in from the left and fades it out when it expires. */
const noticeMotion = {
  enter: { opacity: 0, translate: [-16, 0] },
  exit: { opacity: 0 },
  transition: { seconds: 0.25, easing: 'out' },
} as const

/** view returns what the mod shows player, or an empty view. */
function view(player: Player): View {
  return (
    views.get(player.slot) ?? {
      player,
      root: undefined,
      hud: [],
      cursor: false,
      asks: 0,
      routes: new Map(),
      cues: [],
      bar: undefined,
      actions: new Map(),
      overlay: undefined,
      placement: undefined,
      hudFrame: false,
      display: [],
      notices: [],
    }
  )
}

/**
 * Layer names one independent subtree of a player's interface: the mod's
 * own tree, the chat lines, the notices and the routes. Each lays out and
 * diffs alone, so a change to one costs only its own nodes.
 */
type Layer = 'view' | 'chat' | 'voice' | 'notices' | 'routes'

/** layers lists every layer, the root's children in drawing order first. */
const layers: readonly Layer[] = ['view', 'chat', 'voice', 'notices', 'routes']

/**
 * draw sends what changed in player's interface: the root, which holds the
 * hidden HUD parts, the cursor, the sheets and the sounds, and the changed
 * layers, laid out again while the others keep the nodes they hold. A player
 * with no screen yet gets every layer.
 */
function draw(player: Player, changed: readonly Layer[] = layers): boolean {
  // Lay out the changed layers against the last screen sent.
  const previous = screens.get(player.slot)
  const next = compose(player, previous, previous ? changed : layers)
  const change = diff(previous, next)

  // Send the change; a failed send means the player may have left, so send
  // the screen whole.
  if (change && !send(player.slot, change)) {
    const whole = compose(player, undefined, layers)
    if (!send(player.slot, diff(undefined, whole)!)) {
      screens.delete(player.slot)
      return false
    }
    screens.set(player.slot, whole)
    return true
  }

  // Keep the screen the player now holds.
  screens.set(player.slot, next)
  return true
}

/**
 * drawAll draws every player in the game, as after a change all of them see,
 * laying out only the changed layers.
 */
function drawAll(changed: readonly Layer[] = layers): void {
  for (const { player, away } of players()) {
    if (!away) draw(player, changed)
  }
}

/**
 * compose builds player's screen from previous, laying out the changed
 * layers and keeping the rest as they were.
 */
function compose(
  player: Player,
  previous: Screen | undefined,
  changed: readonly Layer[],
): Screen {
  // Each changed layer continues the screen's id counter.
  const current = view(player)
  const screen: Screen = {
    parts: new Map(previous?.parts),
    root: { node: {}, encoded: '' },
    next: previous?.next ?? 0,
  }
  for (const layer of changed) {
    const layout = new Layout(previous?.parts.get(layer), screen)
    if (layer === 'routes') {
      // A route's id holds a colon, which no element's id does.
      for (const [name, data] of current.routes) {
        layout.add({ id: `route:${name}`, kind: Kind.ROUTE, route: name, data })
      }
    } else {
      layout.part.top = place(layout, content(layer, current), '/')
    }
    screen.parts.set(layer, layout.part)
  }

  // The root is the full-screen panel the layers hang from, and it lists the
  // parts of the game's HUD to hide, whether the cursor is free, the mod's
  // sheets and its last sounds.
  const children: string[] = []
  for (const layer of layers) {
    children.push(...(screen.parts.get(layer)?.top ?? []))
  }
  const root: Node = {
    id: '',
    kind: Kind.PANEL,
    children,
    hideHud: hiddenParts(current.hud),
    freeCursor: current.cursor,
    freeCursorAsks: current.asks,
    sheets: [...sheets],
    cues: [...current.cues],
    abilityBarPresses: current.bar !== undefined,
    gameActions: [...current.actions.keys()].map(
      (action) => gameActions[action],
    ),
    followsOverlay: current.overlay !== undefined,
    followsPlacement: current.placement !== undefined,
    frame: current.hudFrame ? Frame.HUD : Frame.SCREEN,
    abilityDisplay: displayRows(player, current.display),
  }

  // unknown to JSON.stringify, as Part.add explains: scriptc stringifies
  // unknown but not Node's full record type. The root carries no Steam id.
  const value: unknown = root
  screen.root = { node: root, encoded: JSON.stringify(value) }
  return screen
}

/** content is what a layer of current draws. */
function content(layer: Layer, current: View): Child {
  switch (layer) {
    case 'view':
      return current.root
    case 'chat':
      return chatLines(current.player)
    case 'voice':
      return speakers(current.player)
    case 'notices':
      return current.notices.length !== 0 && notices(current.notices)
    default:
      return undefined
  }
}

/** hiddenParts is the wire list of the parts hidden from every player and in own. */
function hiddenParts(own: readonly HudPart[]): WireHudPart[] {
  const parts: WireHudPart[] = []
  for (const part of everyoneHud) parts.push(hudParts[part])
  for (const part of own) {
    const wire = hudParts[part]
    if (!parts.includes(wire)) parts.push(wire)
  }
  return parts
}

/**
 * notices stacks the notices at the lower left, newest at the bottom, clear of
 * the game's health, weapon and spirit stats and the status icons above them.
 */
function notices(stack: readonly Notice[]): Child {
  // The stack fades with its last notice.
  const style: Style = {
    width: 'fit',
    height: 'fit',
    flow: 'down',
    horizontalAlign: 'left',
    verticalAlign: 'bottom',
    margin: [0, 0, 260, 24],
    transition: noticeMotion.transition,
  }
  const labels = stack.map((notice) =>
    jsx(
      'label',
      {
        style: {
          horizontalAlign: 'left',
          margin: [4, 0, 0, 0],
          padding: [5, 10],
          background: '#101418d8',
          color: '#f2f2f2',
          fontSize: 16,
          borderRadius: 4,
          transition: noticeMotion.transition,
        },
        enter: noticeMotion.enter,
        exit: noticeMotion.exit,
        children: notice.text,
      },
      notice.id,
    ),
  )
  return jsx('panel', { style, exit: noticeMotion.exit, children: labels })
}

/**
 * Part is what one layer of a player's interface holds, as the host last
 * accepted it.
 */
interface Part {
  /** nodes maps each node's id to its encoding, to find what changed. */
  readonly nodes: Map<string, { node: Node; encoded: string }>
  /**
   * ids maps each element's place in the tree to its node's id. A place is
   * its parent's place, then "/" for the children, ",index" or ",=key" in an
   * array, and ">" for what a component returned.
   */
  readonly ids: Map<string, string>
  /**
   * presses maps each button's id to its handler, held in a record since
   * scriptc has no Map slot for a function.
   */
  readonly presses: Map<string, { readonly handler: (player: Player) => void }>
  /** changes maps each input's id to its handler, as presses does. */
  readonly changes: Map<string, { readonly handler: ChangeHandler }>
  /** actions maps each layout's id to its press handler, as presses does. */
  readonly actions: Map<string, { readonly handler: LayoutPressHandler }>
  /** documents maps each enabled document input's id to its handlers. */
  readonly documents: Map<string, DocumentHandlers>
  /** wheels holds only the enabled sliders that opted in to wheel events. */
  readonly wheels: Map<
    string,
    { readonly handler: (player: Player, wheel: Wheel) => void }
  >
  /**
   * placed maps each element's place to the element drawn there and what it
   * laid out, so the same element drawn there again reuses it.
   */
  readonly placed: Map<string, Placed>
  /**
   * rows maps each list's id to its rows by key, which the screen sends apart
   * from its nodes, each with its encoding to find what changed.
   */
  readonly rows: Map<string, Map<string, { row: Row; encoded: string }>>
  /** top lists the ids of the layer's outermost nodes, the root's children. */
  top: string[]
}

/** Placed is what one element laid out in a part. */
interface Placed {
  readonly element: UiElement
  /** top lists the ids of the element's outermost nodes, as place returns. */
  readonly top: string[]
  /** places lists the places in the element's subtree that have an id. */
  readonly places: string[]
  /** nodes lists the ids of the nodes in the element's subtree. */
  readonly nodes: string[]
  /** elements lists the places of the elements in the subtree, itself last. */
  readonly elements: string[]
}

/** Screen is what one player's interface holds: the root and its layers. */
interface Screen {
  readonly parts: Map<Layer, Part>
  root: { node: Node; encoded: string }
  /** next numbers the next new node in any layer, so an id is never reused. */
  next: number
}

/** screens holds each player's interface by slot. */
const screens = new Map<number, Screen>()

/** partOf finds the layer of slot's screen that holds the node with id. */
/** DocumentHandlers are a document input's handlers for its two outcomes. */
interface DocumentHandlers {
  readonly submit: (player: Player, text: string) => void
  readonly fail?: (player: Player, failure?: DocumentFailure) => void
}

function partOf(slot: number, id: string): Part | undefined {
  for (const part of screens.get(slot)?.parts.values() ?? []) {
    if (part.nodes.has(id)) return part
  }
  return undefined
}

/**
 * Layout holds one layer while place lays it out. The steps are not closures:
 * a closure that calls itself holds itself, and QuickJS frees such a cycle,
 * with the screens it reaches, only in a full collection that walks the mod's
 * whole heap in one frame. place is a function rather than a method because
 * scriptc cannot hold a method that takes a Child as a prototype value.
 */
class Layout {
  readonly part: Part
  private readonly previous: Part | undefined
  private readonly screen: Screen
  // The logs list, in order, each place given an id, each node added and each
  // element placed, so an element's subtree is the stretch its placing added.
  private readonly placesLog: string[]
  private readonly nodesLog: string[]
  private readonly elementsLog: string[]

  /** The layout keeps previous's ids and numbers new nodes from screen's counter. */
  constructor(previous: Part | undefined, screen: Screen) {
    // Start with empty logs and an empty part.
    this.previous = previous
    this.screen = screen
    this.placesLog = []
    this.nodesLog = []
    this.elementsLog = []
    this.part = {
      nodes: new Map(),
      ids: new Map(),
      presses: new Map(),
      changes: new Map(),
      actions: new Map(),
      documents: new Map(),
      wheels: new Map(),
      placed: new Map(),
      rows: new Map(),
      top: [],
    }
  }

  /** add records a node. */
  add(node: Node): void {
    // Steam id is Node's only bigint. Convert it once instead of calling a
    // replacer for every property of every node and its nested styles. The
    // value is unknown to JSON.stringify, since scriptc stringifies unknown
    // but not Node's full record type.
    const value: unknown =
      node.steamId === undefined
        ? node
        : { ...node, steamId: String(node.steamId) }
    const encoded = JSON.stringify(value)
    this.part.nodes.set(node.id!, { node, encoded })
    this.nodesLog.push(node.id!)
  }

  /**
   * reuse carries over what element laid out at place last time when the
   * previous part drew the same element object there, and returns its top
   * ids; otherwise it returns undefined. The copy touches each node of the
   * subtree once and encodes none of them.
   */
  reuse(element: UiElement, place: string): string[] | undefined {
    // Only the same element object at the same place is unchanged.
    const previous = this.previous
    const placed = previous?.placed.get(place)
    if (!previous || placed?.element !== element) return undefined

    // Carry over its ids, nodes with their handlers, and the elements in it.
    const part = this.part
    for (const at of placed.places) {
      part.ids.set(at, previous.ids.get(at)!)
      this.placesLog.push(at)
    }
    for (const id of placed.nodes) {
      part.nodes.set(id, previous.nodes.get(id)!)
      this.nodesLog.push(id)
      const press = previous.presses.get(id)
      if (press) part.presses.set(id, press)
      const change = previous.changes.get(id)
      if (change) part.changes.set(id, change)
      const action = previous.actions.get(id)
      if (action) part.actions.set(id, action)
      const wheel = previous.wheels.get(id)
      if (wheel) part.wheels.set(id, wheel)
      const document = previous.documents.get(id)
      if (document) part.documents.set(id, document)
      const rows = previous.rows.get(id)
      if (rows) part.rows.set(id, rows)
    }
    for (const at of placed.elements) {
      part.placed.set(at, previous.placed.get(at)!)
      this.elementsLog.push(at)
    }
    return placed.top
  }

  /** mark notes where the logs end, before placing an element. */
  mark(): [number, number, number] {
    return [
      this.placesLog.length,
      this.nodesLog.length,
      this.elementsLog.length,
    ]
  }

  /** placed records that element, laid out at place since mark, made top. */
  placed(
    element: UiElement,
    place: string,
    mark: [number, number, number],
    top: string[],
  ): void {
    this.elementsLog.push(place)
    this.part.placed.set(place, {
      element,
      top,
      places: this.placesLog.slice(mark[0]),
      nodes: this.nodesLog.slice(mark[1]),
      elements: this.elementsLog.slice(mark[2]),
    })
  }

  /** id returns the node id for the element at place. */
  id(place: string): string {
    // Keep the id the place had, or number a new node.
    let found = this.previous?.ids.get(place)
    if (found === undefined) {
      found = (this.screen.next++).toString(36)
    }
    this.part.ids.set(place, found)
    this.placesLog.push(place)
    return found
  }
}

/** place lays out child at place in layout and returns its nodes' ids. */
function place(layout: Layout, child: Child, at: string): string[] {
  // Lay out empty values, lists, text, and the fragments components return.
  if (child === null || child === undefined || typeof child === 'boolean') {
    return []
  }
  if (isList(child)) {
    return child.flatMap((item, index) => {
      const key = isElement(item) ? item.key : undefined
      return place(
        layout,
        item,
        key === undefined ? `${at},${index}` : `${at},=${key}`,
      )
    })
  }
  if (!isElement(child)) {
    const node: Node = {
      id: layout.id(at),
      kind: Kind.LABEL,
      text: String(child),
    }
    layout.add(node)
    return [node.id!]
  }

  // Reuse an element drawn there last time, or lay it out and note what it
  // made.
  const reused = layout.reuse(child, at)
  if (reused) return reused
  const mark = layout.mark()
  const top =
    child.type === fragment
      ? place(layout, child.props.children, `${at}>`)
      : intrinsic(layout, child, at)
  layout.placed(child, at, mark, top)
  return top
}

/** intrinsic lays out an intrinsic element at place in layout. */
function intrinsic(layout: Layout, child: UiElement, at: string): string[] {
  // Build the node's common fields, then its kind's.
  const props = child.props
  const node = denseRecords
    ? denseNode(layout, props, at)
    : sparseNode(layout, props, at)
  switch (child.type) {
    case 'label':
      node.kind = Kind.LABEL
      node.text = text(props.children)
      node.token = props.token
      node.variables = variables(props.variables)
      node.clock = clock(props.clock)
      break
    case 'menu':
      node.kind = Kind.MENU
      node.menu = {
        mode:
          props.mode === 'hold'
            ? MenuMode.HOLD
            : props.mode === 'toggle'
              ? MenuMode.TOGGLE
              : MenuMode.MANUAL,
        openLabel: props.openLabel,
        openRequest: props.openRequest,
        priority: props.priority,
      }
      node.children = place(layout, props.children, `${at}/`)
      break
    case 'menuClose':
      node.kind = Kind.MENU_CLOSE
      node.children = place(layout, props.children, `${at}/`)
      break
    case 'documentInput':
      node.kind = Kind.DOCUMENT_INPUT
      node.documentInput = {
        placeholder: props.placeholder,
        submitLabel: props.submitLabel,
      }
      node.disabled = props.disabled || !props.onSubmit
      if (props.onSubmit && !node.disabled) {
        layout.part.documents.set(node.id!, {
          submit: props.onSubmit,
          fail: props.onFailure,
        })
      }
      break
    case 'documentOutput':
      node.kind = Kind.DOCUMENT_OUTPUT
      node.documentText = props.text
      break
    case 'avatar':
      node.kind = Kind.AVATAR
      node.steamId = props.steamId
      break
    case 'entry':
    case 'slider':
    case 'toggle':
    case 'dropdown':
    case 'range':
      input(layout, node, child.type, props)
      break
    case 'list':
      node.kind = Kind.LIST
      node.list = {
        rowHeight: props.rowHeight,
        row: template(props.row),
        focusStyle: wireStyle(props.focusStyle),
        selectedStyle: wireStyle(props.selectedStyle),
        selected:
          typeof props.selected === 'string' ? props.selected : undefined,
      }
      layout.part.rows.set(node.id!, listRows(props.rows ?? []))
      if (props.onChange) {
        layout.part.changes.set(node.id!, { handler: props.onChange })
      }
      break
    case 'image':
      node.kind = Kind.IMAGE
      node.image = props.src ?? ''
      node.scaling = scaling(props.scaling)
      break
    case 'hero':
      node.kind = Kind.HERO_IMAGE
      node.hero = props.hero
      node.heroLook = props.look === undefined ? undefined : looks[props.look]
      node.scaling = scaling(props.scaling)
      break
    case 'button':
      node.kind = Kind.BUTTON
      node.sound = props.sound
      node.disabled = props.disabled
      if (props.submits) node.submits = true
      focus(node, props)
      // A disabled button's press runs nothing, whoever sends it.
      if (props.onPress && !props.disabled) {
        layout.part.presses.set(node.id!, { handler: props.onPress })
      }
      node.children = place(layout, props.children, `${at}/`)
      break
    case 'layout':
      node.kind = Kind.LAYOUT
      node.layout = props.src ?? ''
      node.data =
        props.data === undefined ? '' : (JSON.stringify(props.data) ?? '')
      if (props.onAction) {
        layout.part.actions.set(node.id!, { handler: props.onAction })
      }
      node.bindings = bindings(layout, node.id!, props.bindings)
      break
    default:
      node.kind = Kind.PANEL
      node.children = place(layout, props.children, `${at}/`)
  }
  layout.add(node)
  return [node.id!]
}

/**
 * template lays row out as a list's row template: its own nodes, the root
 * named "".
 */
function template(row: Child): Node[] {
  // Lay the row out alone, then rename its root.
  const own = new Layout(undefined, {
    parts: new Map(),
    root: { node: {}, encoded: '' },
    next: 0,
  })
  const top = place(own, row, '')
  if (top.length !== 1) return []
  const nodes = [...own.part.nodes.values()].map(({ node }) => node)
  for (const node of nodes) {
    if (node.id === top[0]) node.id = ''
  }
  return nodes
}

/** listRows keeps a list's rows by key; of two with one key, the last counts. */
function listRows(
  rows: readonly ListRow[],
): Map<string, { row: Row; encoded: string }> {
  const kept = new Map<string, { row: Row; encoded: string }>()
  for (const { key, rank, sort, fields } of rows) {
    const row: Row = { key, rank, sort, fields: fields && { ...fields } }
    kept.set(key, { row, encoded: JSON.stringify(row) })
  }
  return kept
}

/**
 * listChanges returns the changes that bring each list before holds to its
 * rows in after: every row of a list new to the screen, and the rows added,
 * changed and removed of one it holds.
 */
function listChanges(
  before: Part | undefined,
  after: Part | undefined,
): ListChange[] {
  const changes: ListChange[] = []
  for (const [node, rows] of after?.rows ?? []) {
    const held = before?.rows.get(node)
    if (held === rows) continue
    const set: Row[] = []
    for (const [key, { row, encoded }] of rows) {
      if (held?.get(key)?.encoded !== encoded) set.push(row)
    }
    const removed = [...(held?.keys() ?? [])].filter((key) => !rows.has(key))
    if (held && set.length === 0 && removed.length === 0) continue
    changes.push({ node, reset: !held, set, removed })
  }
  return changes
}

/** sparseNode builds a node's common fields, only those props sets. */
function sparseNode(layout: Layout, props: Props, at: string): Node {
  // Name the node and style it.
  const node: Node = { id: layout.id(at) }
  if (props.style !== undefined) node.style = wireStyle(props.style)
  if (props.hover !== undefined) node.hover = wireStyle(props.hover)
  if (props.enter !== undefined) node.enter = wireStyle(props.enter)
  if (props.exit !== undefined) node.exit = wireStyle(props.exit)
  if (props.cycle !== undefined) node.cycle = cycle(props.cycle)
  if (props.class !== undefined) node.classes = classes(props.class)

  // Give it what it shows on hover, the item or ability it names, and
  // whether chat hides it.
  if (props.tooltip !== undefined) node.tooltip = props.tooltip
  if (props.hoverSound !== undefined) node.hoverSound = props.hoverSound
  if (props.measure !== undefined) node.measure = props.measure
  if (props.item) node.item = itemId(props.item)
  if (props.ability?.name) {
    node.ability = itemId(props.ability.name)
    node.abilityPlayer = props.ability.player.slot
  }
  if (props.hideInChat) node.hideInChat = true
  if (props.speaking) node.speaking = speakingName(props.speaking)
  return node
}

/** denseNode builds a node's common fields, every one present (useDenseUiRecords). */
function denseNode(layout: Layout, props: Props, at: string): Node {
  return {
    id: layout.id(at),
    style: wireStyle(props.style),
    hover: wireStyle(props.hover),
    tooltip: props.tooltip,
    item: props.item ? itemId(props.item) : undefined,
    ability: props.ability?.name ? itemId(props.ability.name) : undefined,
    abilityPlayer: props.ability?.name ? props.ability.player.slot : undefined,
    hoverSound: props.hoverSound,
    enter: wireStyle(props.enter),
    exit: wireStyle(props.exit),
    cycle: cycle(props.cycle),
    classes: classes(props.class),
    hideInChat: props.hideInChat || undefined,
    speaking: props.speaking ? speakingName(props.speaking) : undefined,
  }
}

/**
 * speakingName is the name the player's game knows a speaker by: their
 * name in its top bar, which is their connection's. A speaker with no name
 * yet gets one nobody holds, so the element stays hidden.
 */
function speakingName(player: Player): string {
  return connection(player)?.name || `#${player.slot}`
}

/** classes splits an element's class into its names, or undefined for none. */
function classes(names: string | undefined): string[] | undefined {
  const list = (names ?? '').split(' ').filter((name) => name !== '')
  return list.length === 0 ? undefined : list
}

/** focus sets a control's place in the navigation and its initial focus. */
function focus(node: Node, props: Props): void {
  if (props.focusOrder)
    node.focusOrder = Math.max(0, Math.trunc(props.focusOrder))
  if (props.initialFocus) node.initialFocus = true
}

/** input lays out an input of kind from props in node. */
function input(
  layout: Layout,
  node: Node,
  kind: 'entry' | 'slider' | 'toggle' | 'dropdown' | 'range',
  props: Props,
): void {
  // A disabled input's change runs nothing, whoever sends it.
  node.kind = Kind.INPUT
  node.disabled = props.disabled
  focus(node, props)
  if (kind === 'range' && !props.disabled) {
    if (props.onChange || props.onHold) {
      layout.part.changes.set(node.id!, { handler: held(props) })
    }
  } else if (props.onChange && !props.disabled) {
    layout.part.changes.set(node.id!, { handler: props.onChange })
  }

  // Each control carries its value and its bounds.
  switch (kind) {
    case 'entry':
      node.input = {
        case: 'textEntry',
        value: {
          text: String(props.value ?? ''),
          placeholder: props.placeholder,
          maxLength: props.maxLength,
        },
      }
      break
    case 'slider':
      if (props.onWheel && !props.disabled) {
        layout.part.wheels.set(node.id!, { handler: props.onWheel })
      }
      node.input = {
        case: 'slider',
        value: {
          value: Number(props.value ?? 0),
          min: props.min,
          max: props.max,
          step: props.step,
          wheel: !!props.onWheel && !props.disabled,
        },
      }
      break
    case 'toggle':
      node.text = text(props.children)
      node.input = { case: 'toggle', value: { on: props.on } }
      break
    case 'dropdown':
      node.input = {
        case: 'dropdown',
        value: {
          options: [...(props.options ?? [])],
          selected:
            typeof props.selected === 'number' ? props.selected : undefined,
        },
      }
      break
    case 'range':
      node.input = {
        case: 'range',
        value: {
          handles: (props.handles ?? []).map((handle) => ({
            name: handle.name,
            value: Number(handle.value),
          })),
          min: props.min,
          max: props.max,
          step: props.step,
        },
      }
  }
}

/**
 * held returns a range's change handler: each hold reaches onHold, and a
 * release also reaches onChange with every handle's value.
 */
function held(props: Props): ChangeHandler {
  return (player: Player, event: RangeEvent) => {
    if (event.phase) props.onHold?.(player, event.handle, event.phase)
    if (event.phase === 'released') props.onChange?.(player, event.values)
  }
}

/** clock encodes a label's clock. */
function clock(value: Clock | undefined): WireClock | undefined {
  if (!value) {
    return undefined
  }
  return {
    seconds: value.seconds,
    up: value.up,
    paused: value.paused,
    format: value.format === undefined ? undefined : clockFormats[value.format],
    run: value.run,
  }
}

/** clockFormats maps each clock format to its wire value. */
const clockFormats = {
  minutes: ClockFormat.MINUTES,
  seconds: ClockFormat.SECONDS,
  tenths: ClockFormat.TENTHS,
}

/** clip encodes a clip: a box when rect is set, otherwise a slice. */
function clip(value: Clip | undefined): WireClip | undefined {
  if (!value) {
    return undefined
  }
  if (value.rect) {
    const [top, right, bottom, left] = value.rect
    return { shape: { case: 'rect', value: { top, right, bottom, left } } }
  }
  return {
    shape: {
      case: 'radial',
      value: {
        centerX: value.center?.[0],
        centerY: value.center?.[1],
        start: value.start,
        sweep: value.sweep,
      },
    },
  }
}

/**
 * bindings encodes a layout's bindings in their panels' order, so a redraw
 * sends the same node, and keeps each bound press's handler in the part's
 * presses under boundKey.
 */
function bindings(
  layout: Layout,
  node: string,
  value: Readonly<Record<string, Binding>> | undefined,
): BindingNode[] {
  // Encode each binding, its press as a flag.
  const encoded: BindingNode[] = []
  for (const [panel, binding] of Object.entries(value ?? {}).sort(([a], [b]) =>
    a < b ? -1 : a > b ? 1 : 0,
  )) {
    encoded.push({
      panel,
      text: binding.text,
      token: binding.token,
      variables: variables(binding.variables),
      visible: binding.visible,
      classes: classes(binding.class),
      style: wireStyle(binding.style),
      image: binding.src,
      press: binding.onPress !== undefined,
    })

    // Keep the press's handler.
    if (binding.onPress) {
      layout.part.presses.set(boundKey(node, panel), {
        handler: binding.onPress,
      })
    }
  }
  return encoded
}

/**
 * boundKey names a layout's bound panel among a part's presses: no button's
 * id holds a space.
 */
function boundKey(node: string, panel: string): string {
  return `${node} ${panel}`
}

/** variables encodes a localized label's variables as text. */
function variables(
  value: Readonly<Record<string, string | number>> | undefined,
): Record<string, string> | undefined {
  if (!value) {
    return undefined
  }
  const encoded: Record<string, string> = {}
  for (const [name, filled] of Object.entries(value)) {
    encoded[name] = String(filled)
  }
  return encoded
}

/** cycle encodes a cycle. */
function cycle(value: Cycle | undefined): WireCycle | undefined {
  if (!value) {
    return undefined
  }
  return {
    style: wireStyle(value.style),
    seconds: value.seconds,
    count: value.count,
  }
}

/**
 * diff returns the change from previous to next, or undefined when nothing
 * changed. A layer next shares with previous is unchanged and not walked.
 */
function diff(previous: Screen | undefined, next: Screen): Change | undefined {
  // The root goes when it changed, and each changed layer sends its new and
  // changed nodes and removes the ones it no longer holds.
  const set: Node[] = []
  const removed: string[] = []
  const lists: ListChange[] = []
  if (previous?.root.encoded !== next.root.encoded) set.push(next.root.node)
  for (const layer of layers) {
    const before = previous?.parts.get(layer)
    const after = next.parts.get(layer)
    if (before === after) continue
    for (const { node, encoded } of after?.nodes.values() ?? []) {
      if (before?.nodes.get(node.id!)?.encoded !== encoded) set.push(node)
    }
    for (const id of before?.nodes.keys() ?? []) {
      if (!after?.nodes.has(id)) removed.push(id)
    }
    lists.push(...listChanges(before, after))
  }
  if (
    previous &&
    set.length === 0 &&
    removed.length === 0 &&
    lists.length === 0
  ) {
    return undefined
  }
  return { reset: !previous, set, removed, lists }
}

/** send asks the host to apply change to the player in slot. */
function send(slot: number, change: Change): boolean {
  return call('Ui', UiRequest.toBinary({ player: slot, change })) !== undefined
}

/** isElement reports whether child is an element. */
function isElement(child: Child): child is UiElement {
  return typeof child === 'object' && child !== null && !Array.isArray(child)
}

/**
 * isList reports whether a child is a list of children. Array.isArray does not
 * narrow a readonly array type, so the list case needs its own guard.
 */
function isList(child: Child): child is readonly Child[] {
  return Array.isArray(child)
}

/** text joins a label's text children. */
function text(children: Child): string {
  if (
    children === null ||
    children === undefined ||
    typeof children === 'boolean'
  ) {
    return ''
  }
  if (isList(children)) {
    return children.map(text).join('')
  }
  return isElement(children) ? '' : String(children)
}

/** aligns maps each alignment to its wire value. */
const aligns = {
  left: Align.START,
  top: Align.START,
  center: Align.CENTER,
  right: Align.END,
  bottom: Align.END,
} as const

/** align encodes an alignment. */
function align(value: keyof typeof aligns | undefined): Align | undefined {
  return value === undefined ? undefined : aligns[value]
}

/** gameActions maps each game action to its wire value. */
const gameActions: Record<GameAction, WireGameAction> = {
  scoreboard: WireGameAction.SCOREBOARD,
}

/** hudParts maps each part of the game's HUD to its wire value. */
const hudParts = {
  levelUp: WireHudPart.LEVEL_UP,
  ownHealth: WireHudPart.OWN_HEALTH,
  topBar: WireHudPart.TOP_BAR,
  ammo: WireHudPart.AMMO,
  souls: WireHudPart.SOULS,
  minimap: WireHudPart.MINIMAP,
  abilities: WireHudPart.ABILITIES,
  status: WireHudPart.STATUS,
  stats: WireHudPart.STATS,
  heroPicker: WireHudPart.HERO_PICKER,
  brokerCountdown: WireHudPart.BROKER_COUNTDOWN,
  minimapPlayers: WireHudPart.MINIMAP_PLAYERS,
  inventory: WireHudPart.INVENTORY,
  crosshair: WireHudPart.CROSSHAIR,
  castFailed: WireHudPart.CAST_FAILED,
  eventIndicators: WireHudPart.EVENT_INDICATORS,
  statusIndicators: WireHudPart.STATUS_INDICATORS,
  all: WireHudPart.ALL,
} as const

/** faces maps each typeface to its wire value. */
const faces = {
  body: Face.BODY,
  block: Face.BLOCK,
  serif: Face.SERIF,
} as const

/** looks maps each picture of a hero to its wire value. */
const looks = {
  small: HeroLook.SMALL,
  card: HeroLook.CARD,
  icon: HeroLook.ICON,
  vertical: HeroLook.VERTICAL,
  topBar: HeroLook.TOP_BAR,
  minimap: HeroLook.MINIMAP,
} as const

/** scalings maps each way to fit a picture to its wire value. */
const scalings = {
  fit: WireScaling.FIT,
  cover: WireScaling.COVER,
  stretch: WireScaling.STRETCH,
  fitWidth: WireScaling.FIT_WIDTH,
  fitHeight: WireScaling.FIT_HEIGHT,
  none: WireScaling.NONE,
} as const

/** scaling encodes a way to fit a picture. */
function scaling(value: Scaling | undefined): WireScaling | undefined {
  return value === undefined ? undefined : scalings[value]
}

/** flows maps each flow to its wire value. */
const flows = {
  down: Flow.DOWN,
  right: Flow.RIGHT,
  downWrap: Flow.DOWN_WRAP,
  rightWrap: Flow.RIGHT_WRAP,
} as const

/** overflows maps each overflow to its wire value. */
const overflows = {
  squish: WireOverflow.SQUISH,
  clip: WireOverflow.CLIP,
  scroll: WireOverflow.SCROLL,
  show: WireOverflow.SHOW,
} as const

/** overflow encodes an overflow. */
function overflow(value: Overflow | undefined): WireOverflow | undefined {
  return value === undefined ? undefined : overflows[value]
}

/** shapes maps each gradient shape to its wire value. */
const shapes = {
  down: GradientShape.DOWN,
  right: GradientShape.RIGHT,
  up: GradientShape.UP,
  left: GradientShape.LEFT,
  radial: GradientShape.RADIAL,
} as const

/** weights maps each text weight to its wire value. */
const weights = {
  thin: Weight.THIN,
  light: Weight.LIGHT,
  normal: Weight.NORMAL,
  medium: Weight.MEDIUM,
  semiBold: Weight.SEMI_BOLD,
  bold: Weight.BOLD,
  black: Weight.BLACK,
} as const

/** textOverflows maps each text overflow to its wire value. */
const textOverflows = {
  ellipsis: TextOverflow.ELLIPSIS,
  shrink: TextOverflow.SHRINK,
  clip: TextOverflow.CLIP,
} as const

/** decorations maps each text decoration to its wire value. */
const decorations = {
  underline: Decoration.UNDERLINE,
  strike: Decoration.STRIKE,
} as const

/** easings maps each easing to its wire value. */
const easings = {
  out: Easing.EASE_OUT,
  in: Easing.EASE_IN,
  inOut: Easing.EASE_IN_OUT,
  linear: Easing.LINEAR,
} as const

/**
 * denseRecords lays each node and style out with every field present, most of
 * them undefined, as the interface did before it built them sparse. The
 * encoding and the bytes sent are the same either way; useDenseUiRecords
 * selects it to compare.
 */
let denseRecords = false

/**
 * useDenseUiRecords makes the interface build each node and style record with
 * every field present, as it once did, instead of only the fields set. The
 * screens and the bytes sent do not change; only the cost of building and
 * encoding them does, so this exists to compare the two and to step back.
 */
export function useDenseUiRecords(dense: boolean): void {
  denseRecords = dense
}

/**
 * wireStyle encodes style with only the fields style sets. Its records hold
 * them in another order than denseStyle's, which the bytes sent do not show:
 * protobuf writes fields in number order, and the encodings the diff compares
 * come from one builder at a time.
 */
function wireStyle(style: Style | undefined): WireStyle | undefined {
  // No style, or the dense form, needs no sparse record.
  if (!style) {
    return undefined
  }
  if (denseRecords) {
    return denseStyle(style)
  }
  const out: WireStyle = {}

  // Size the box.
  if (style.width !== undefined) out.width = length(style.width)
  if (style.height !== undefined) out.height = length(style.height)
  if (style.minWidth !== undefined) out.minWidth = length(style.minWidth)
  if (style.maxWidth !== undefined) out.maxWidth = length(style.maxWidth)
  if (style.minHeight !== undefined) out.minHeight = length(style.minHeight)
  if (style.maxHeight !== undefined) out.maxHeight = length(style.maxHeight)

  // Place it and its children.
  if (style.x !== undefined) out.x = length(style.x)
  if (style.y !== undefined) out.y = length(style.y)
  if (style.zIndex !== undefined) out.zIndex = style.zIndex
  if (style.flow !== undefined) out.flow = flows[style.flow]
  if (style.horizontalAlign !== undefined) {
    out.horizontalAlign = align(style.horizontalAlign)
  }
  if (style.verticalAlign !== undefined) {
    out.verticalAlign = align(style.verticalAlign)
  }
  if (style.margin !== undefined) out.margin = edges(style.margin)
  if (style.padding !== undefined) out.padding = edges(style.padding)

  // Clip and turn it.
  const overflowX = style.overflowX ?? style.overflow
  if (overflowX !== undefined) out.overflowX = overflow(overflowX)
  const overflowY = style.overflowY ?? style.overflow
  if (overflowY !== undefined) out.overflowY = overflow(overflowY)
  if (style.clip !== undefined) out.clip = clip(style.clip)
  if (style.turn !== undefined) out.turn = style.turn

  // Paint the box.
  if (style.background !== undefined) out.background = color(style.background)
  if (style.borderRadius !== undefined) out.borderRadius = style.borderRadius
  if (style.borderWidth !== undefined) out.borderWidth = style.borderWidth
  if (style.borderColor !== undefined) {
    out.borderColor = color(style.borderColor)
  }
  if (style.opacity !== undefined) out.opacity = style.opacity
  if (style.gradient !== undefined) out.gradient = gradient(style.gradient)
  if (style.boxShadow !== undefined) out.boxShadow = shadow(style.boxShadow)
  if (style.tint !== undefined) out.tint = color(style.tint)

  // Filter what it draws.
  if (style.saturation !== undefined) out.saturation = style.saturation
  if (style.brightness !== undefined) out.brightness = style.brightness
  if (style.contrast !== undefined) out.contrast = style.contrast
  if (style.blur !== undefined) out.blur = style.blur

  // Set the text's color, size and face.
  if (style.color !== undefined) out.color = color(style.color)
  if (style.fontSize !== undefined) out.fontSize = style.fontSize
  if (style.bold !== undefined) out.bold = style.bold
  if (style.textAlign !== undefined) out.textAlign = align(style.textAlign)
  if (style.face !== undefined) out.face = faces[style.face]
  if (style.uppercase !== undefined) out.uppercase = style.uppercase
  if (style.letterSpacing !== undefined) {
    out.letterSpacing = style.letterSpacing
  }
  if (style.shadow !== undefined) out.shadow = style.shadow

  // Set the text's weight, lines and marks.
  if (style.italic !== undefined) out.italic = style.italic
  if (style.weight !== undefined) out.weight = weights[style.weight]
  if (style.lineHeight !== undefined) out.lineHeight = style.lineHeight
  if (style.noWrap !== undefined) out.noWrap = style.noWrap
  if (style.textOverflow !== undefined) {
    out.textOverflow = textOverflows[style.textOverflow]
  }
  if (style.decoration !== undefined) {
    out.decoration = decorations[style.decoration]
  }
  if (style.textShadow !== undefined) out.textShadow = shadow(style.textShadow)

  // Move and animate it.
  const moved = transform(style)
  if (moved !== undefined) out.transform = moved
  if (style.transition !== undefined) {
    out.transition = transition(style.transition)
  }
  return out
}

/** denseStyle encodes style with every field present (useDenseUiRecords). */
function denseStyle(style: Style): WireStyle {
  return {
    width: length(style.width),
    height: length(style.height),
    flow: style.flow === undefined ? undefined : flows[style.flow],
    horizontalAlign: align(style.horizontalAlign),
    verticalAlign: align(style.verticalAlign),
    margin: edges(style.margin),
    padding: edges(style.padding),
    background: color(style.background),
    color: color(style.color),
    fontSize: style.fontSize,
    bold: style.bold,
    textAlign: align(style.textAlign),
    borderRadius: style.borderRadius,
    opacity: style.opacity,
    face: style.face === undefined ? undefined : faces[style.face],
    uppercase: style.uppercase,
    letterSpacing: style.letterSpacing,
    shadow: style.shadow,
    borderWidth: style.borderWidth,
    borderColor: color(style.borderColor),
    x: length(style.x),
    y: length(style.y),
    minWidth: length(style.minWidth),
    maxWidth: length(style.maxWidth),
    minHeight: length(style.minHeight),
    maxHeight: length(style.maxHeight),
    zIndex: style.zIndex,
    overflowX: overflow(style.overflowX ?? style.overflow),
    overflowY: overflow(style.overflowY ?? style.overflow),
    gradient: gradient(style.gradient),
    boxShadow: shadow(style.boxShadow),
    textShadow: shadow(style.textShadow),
    tint: color(style.tint),
    saturation: style.saturation,
    brightness: style.brightness,
    contrast: style.contrast,
    blur: style.blur,
    italic: style.italic,
    weight: style.weight === undefined ? undefined : weights[style.weight],
    lineHeight: style.lineHeight,
    noWrap: style.noWrap,
    textOverflow:
      style.textOverflow === undefined
        ? undefined
        : textOverflows[style.textOverflow],
    decoration:
      style.decoration === undefined
        ? undefined
        : decorations[style.decoration],
    transform: transform(style),
    turn: style.turn,
    clip: clip(style.clip),
    transition: transition(style.transition),
  }
}

/** gradient encodes a gradient, spacing colors without places evenly. */
function gradient(value: Gradient | undefined): WireGradient | undefined {
  if (!value) {
    return undefined
  }
  const last = Math.max(1, value.colors.length - 1)
  return {
    shape: value.shape === undefined ? undefined : shapes[value.shape],
    stops: value.colors.map((each, index) => ({
      at: value.at?.[index] ?? index / last,
      color: color(each),
    })),
  }
}

/** shadow encodes a shadow. */
function shadow(value: Shadow | undefined): WireShadow | undefined {
  if (!value) {
    return undefined
  }
  return {
    x: value.x,
    y: value.y,
    blur: value.blur,
    spread: value.spread,
    color: color(value.color),
    inset: value.inset,
  }
}

/** transform encodes a style's move, scale, turn and origin, or none. */
function transform(style: Style): WireTransform | undefined {
  const { translate, scale, rotate, origin } = style
  if (!translate && scale === undefined && rotate === undefined && !origin) {
    return undefined
  }
  const scales = typeof scale === 'number' ? [scale, scale] : scale
  return {
    translateX: translate?.[0],
    translateY: translate?.[1],
    scaleX: scales?.[0],
    scaleY: scales?.[1],
    rotate,
    originX: origin?.[0],
    originY: origin?.[1],
  }
}

/** transition encodes a transition: seconds, or a Transition. */
function transition(
  value: number | Transition | undefined,
): WireTransition | undefined {
  if (value === undefined) {
    return undefined
  }
  if (typeof value === 'number') {
    return { seconds: value }
  }
  return {
    seconds: value.seconds,
    delay: value.delay,
    easing: value.easing === undefined ? undefined : easings[value.easing],
    curve: value.curve
      ? {
          x1: value.curve[0],
          y1: value.curve[1],
          x2: value.curve[2],
          y2: value.curve[3],
        }
      : undefined,
  }
}

/** length encodes a length. */
function length(value: Length | undefined): WireLength | undefined {
  // Convert each length form to its wire case.
  if (value === undefined) {
    return undefined
  }
  if (typeof value === 'number') {
    return { value: { case: 'pixels', value } }
  }
  if (value === 'fit') {
    return { value: { case: 'fitChildren', value: true } }
  }
  if (value === 'fill') {
    return { value: { case: 'fill', value: 1 } }
  }
  return { value: { case: 'percent', value: Number.parseFloat(value) } }
}

/** edges encodes edges. */
function edges(value: Edges | undefined): WireEdges | undefined {
  if (value === undefined) {
    return undefined
  }
  if (typeof value === 'number') {
    return { top: value, right: value, bottom: value, left: value }
  }
  if (value.length === 2) {
    return { top: value[0], right: value[1], bottom: value[0], left: value[1] }
  }
  return { top: value[0], right: value[1], bottom: value[2], left: value[3] }
}

/**
 * colors caches each color string's encoding, since a screen names the same few
 * colors on many nodes; it empties when it reaches maxColors, so a mod that
 * computes its colors cannot grow it without bound.
 */
const colors = new Map<string, number>()
const maxColors = 1024

/** color encodes '#rrggbb' or '#rrggbbaa' as 0xRRGGBBAA. */
function color(value: string | undefined): number | undefined {
  // No color, or the dense form, skips the cache.
  if (value === undefined) return undefined
  if (denseRecords) return parseColor(value)

  // A string that is no color is cached as -1, which no color encodes to.
  let parsed = colors.get(value)
  if (parsed === undefined) {
    if (colors.size >= maxColors) colors.clear()
    parsed = parseColor(value) ?? -1
    colors.set(value, parsed)
  }
  return parsed < 0 ? undefined : parsed
}

/** parseColor parses '#rrggbb' or '#rrggbbaa', or returns undefined. */
function parseColor(value: string): number | undefined {
  const hex = value.replace(/^#/, '')
  if (!hex || !/^[0-9a-f]{6}([0-9a-f]{2})?$/i.test(hex)) {
    return undefined
  }
  return Number.parseInt(hex.length === 6 ? `${hex}ff` : hex, 16) >>> 0
}
