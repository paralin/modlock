--!strict
-- ui draws each player's interface from a tree of elements. Each element is
-- one table: its props under their names and its children in its list part.
-- A component is a plain function that returns an element.
--
--   local ui = require("@modlock/ui")
--
--   ui.show(player, ui.panel { style = { flow = "down" },
--   	ui.label { style = { fontSize = 32 }, `Round {round}` },
--   	ui.button { onPress = ready, "Ready" },
--   })
--
-- The engine lays the tree out as flat nodes and sends the host only the
-- nodes that changed since the screen the player holds. A node keeps its id
-- while its element keeps its place in the tree; a list child with a key
-- keeps its place by the key, wherever it moves in the list.

local modlock = require("@modlock")

-- Player and Wheel are the library's types the interface takes.
type Player = modlock.Player
type Wheel = modlock.Wheel

local ui = {}

-- Length is a size: pixels, a percentage of the parent such as "50%", "fit"
-- to size to the children, or "fill" to share the space left in the parent's
-- flow.
export type Length = number | string

-- Edges are distances in pixels: one for every side, { vertical, horizontal },
-- or { top, right, bottom, left }.
export type Edges = number | { number }

-- Overflow decides what happens to children that do not fit: "squish" shrinks
-- them, "clip" cuts them off, "scroll" lets the player scroll to them, and
-- "show" draws them outside the element.
export type Overflow = "squish" | "clip" | "scroll" | "show"

-- Gradient blends colors across an element in place of its background. The
-- colors run down by default; at places each color from 0 to 1, spaced evenly
-- when unset.
export type Gradient = {
	-- shape is the direction the colors run, or "radial" from the center.
	shape: ("down" | "right" | "up" | "left" | "radial")?,
	-- colors are the colors in order.
	colors: { string },
	-- at places each color, from 0 to 1.
	at: { number }?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Shadow is a soft shadow, offset by x and y and softened by blur, in pixels.
-- Spread grows a box's shadow in pixels, or strengthens a text shadow, where
-- 0 draws it at full strength; inset draws a box's shadow inside its edge.
export type Shadow = {
	-- x and y offset the shadow.
	x: number?,
	y: number?,
	-- blur softens it.
	blur: number?,
	-- spread grows or strengthens it.
	spread: number?,
	-- color is its color.
	color: string,
	-- inset draws it inside the box's edge.
	inset: boolean?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Transition animates each change of an element's style, and the move to its
-- hover style and back, over seconds after delay. Easing is "out" unless set.
export type Transition = {
	-- seconds is how long each change takes.
	seconds: number,
	-- delay is how long each change waits first.
	delay: number?,
	-- easing shapes the change's speed.
	easing: ("out" | "in" | "inOut" | "linear")?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Style lays out and paints an element. Colors are "#rrggbb" or "#rrggbbaa".
export type Style = {
	-- width and height size the element.
	width: Length?,
	height: Length?,
	-- minWidth, maxWidth, minHeight and maxHeight bound the element's size.
	minWidth: Length?,
	maxWidth: Length?,
	minHeight: Length?,
	maxHeight: Length?,
	-- flow places the children in a column or a row, or in columns or rows
	-- that wrap where one is full; unset stacks them.
	flow: ("down" | "right" | "downWrap" | "rightWrap")?,
	-- x and y place the element at an offset from its parent's top left
	-- corner, in pixels or a percent of the parent, outside the parent's flow.
	x: Length?,
	y: Length?,
	-- zIndex draws the element above its siblings with a lower index.
	zIndex: number?,
	-- overflow decides what children that do not fit do, across and down.
	overflow: Overflow?,
	-- overflowX and overflowY set overflow for one direction.
	overflowX: Overflow?,
	overflowY: Overflow?,
	-- horizontalAlign and verticalAlign place the element in its parent.
	horizontalAlign: ("left" | "center" | "right")?,
	verticalAlign: ("top" | "center" | "bottom")?,
	-- margin is the space outside the element, padding the space inside.
	margin: Edges?,
	padding: Edges?,
	-- background fills the element, and color colors its text.
	background: string?,
	color: string?,
	-- fontSize is the text's size in pixels.
	fontSize: number?,
	-- bold sets the text in bold.
	bold: boolean?,
	-- textAlign aligns the text in the element.
	textAlign: ("left" | "center" | "right")?,
	-- borderRadius rounds the corners, in pixels.
	borderRadius: number?,
	-- opacity fades the element and its children, from 0 to 1.
	opacity: number?,
	-- face is one of the game's typefaces: "body" its text, "block" the
	-- capitals of its clock and titles, "serif" its menus.
	face: ("body" | "block" | "serif")?,
	-- uppercase sets the text in capitals.
	uppercase: boolean?,
	-- letterSpacing widens the space between letters, in pixels.
	letterSpacing: number?,
	-- shadow draws a soft drop shadow under the text, to read over the world.
	shadow: boolean?,
	-- textShadow draws this shadow under the text in place of shadow's.
	textShadow: Shadow?,
	-- borderWidth and borderColor draw a line around the element's edge; it
	-- shows only when both are set.
	borderWidth: number?,
	borderColor: string?,
	-- gradient fills the element in place of its background.
	gradient: Gradient?,
	-- boxShadow draws a shadow around or inside the element's box.
	boxShadow: Shadow?,
	-- tint washes the element and its children with a color, recoloring a
	-- white icon.
	tint: string?,
	-- saturation, brightness and contrast scale the colors; 1 leaves them.
	saturation: number?,
	brightness: number?,
	contrast: number?,
	-- blur blurs the element and its children by a radius in pixels.
	blur: number?,
	-- italic slants the text.
	italic: boolean?,
	-- weight sets the text's weight in place of bold.
	weight: ("thin" | "light" | "normal" | "medium" | "semiBold" | "bold" | "black")?,
	-- lineHeight is the height of each line of text, in pixels.
	lineHeight: number?,
	-- noWrap keeps the text on one line.
	noWrap: boolean?,
	-- textOverflow ends text that does not fit with an "ellipsis", "shrink"s
	-- it until it fits, or "clip"s it at the edge.
	textOverflow: ("ellipsis" | "shrink" | "clip")?,
	-- decoration draws a line under or through the text.
	decoration: ("underline" | "strike")?,
	-- translate moves the element as drawn by { x, y } pixels.
	translate: { number }?,
	-- scale scales the element as drawn: one factor, or { x, y }.
	scale: (number | { number })?,
	-- rotate turns the element clockwise, in degrees.
	rotate: number?,
	-- origin is the point the element scales and turns about, as { x, y }
	-- percents of its size; unset is its center.
	origin: { number }?,
	-- transition animates each change of the style: seconds, or a Transition.
	-- Animate transform, opacity and colors; size and placement animate too,
	-- but move the element's neighbors each frame.
	transition: (number | Transition)?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- HudPart is a part of the game's own HUD: "levelUp" the notice for each
-- boon, ability point and power increase, "ownHealth" the player's health bar
-- with its shields and regeneration, "topBar" the bar of team scores,
-- portraits and clock across the top, "ammo" the clip ring, count and ammo
-- warnings at the crosshair, "souls" the souls and level readout, "minimap"
-- the minimap with its hints and district name, "abilities" the ability and
-- active item bar, "status" the game's mark in the top-left corner, "stats"
-- the weapon, spirit and vitality stats at the lower left, "heroPicker" the
-- screen a player without a hero picks one on, "brokerCountdown" the
-- countdown in the Broker's window, and "all" the whole HUD but the mods' own
-- interfaces and the game's tooltips, for a screen that takes the player's
-- whole view. Whatever a part does not name stays, such as the crosshair and
-- chat.
export type HudPart =
	"levelUp"
	| "ownHealth"
	| "topBar"
	| "ammo"
	| "souls"
	| "minimap"
	| "abilities"
	| "status"
	| "stats"
	| "heroPicker"
	| "brokerCountdown"
	| "all"

-- Element is one element of an interface, as its function builds it.
export type Element = {
	-- type names the element, such as "panel".
	type: string,
	-- props are the table the element was built from.
	props: { [any]: any },
	-- key keeps the element's place in a list.
	key: string?,
}

-- Child is anything an element may hold: an element, text, which becomes a
-- label, a list of children, or false or nil for nothing. A list holds any
-- values, so a typed list such as { Element } passes as it is; each element
-- in it was checked as its function built it.
export type Child = Element | string | number | boolean | { any }

-- AbilityTip is the game's own window for an ability, in the player's
-- language: name is the ability's class name, such as "ability_bebop_hook",
-- and player is whose hero the window reads its numbers from, a hero that
-- stands on the map, such as a bot that plays it.
export type AbilityTip = {
	-- name is the ability's class name.
	name: string,
	-- player is whose hero the window reads.
	player: Player,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Cycle swings an element from its style to style and back, each way taking
-- seconds, count times or, without a count, while it shows.
export type Cycle = {
	-- style is the style the element swings to.
	style: Style,
	-- seconds is how long each way takes.
	seconds: number,
	-- count is how many times it swings.
	count: number?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Scaling fits a picture into its element's box: "fit" shows it whole,
-- "cover" fills the box and crops, "stretch" fills the box and stretches,
-- "fitWidth" and "fitHeight" fit one side, and "none" draws it at its own
-- size.
export type Scaling = "fit" | "cover" | "stretch" | "fitWidth" | "fitHeight" | "none"

-- HeroLook is one of the game's pictures of a hero: "small" the square
-- portrait of the kill feed, "card" the hero picker's tall card, "icon" the
-- round icon, "vertical" the hero grid's narrow portrait, "topBar" the top
-- bar's portrait and "minimap" the minimap icon.
export type HeroLook = "small" | "card" | "icon" | "vertical" | "topBar" | "minimap"

-- Each element's props below repeat the props every drawn element takes, so
-- a strict mod's table is checked against one sealed type: class names the
-- element's classes in the mod's sheets, separated by spaces; hover is the
-- style drawn over style while the pointer is over the element; tooltip is
-- the text the game's tooltip shows then, item names an item by class name,
-- such as "upgrade_ability_power_shard", whose shop card shows instead,
-- ability names an ability whose window shows instead of either, and
-- hoverSound is the game's sound event played as the pointer enters, such as
-- "UI.MainMenu.Hover"; enter is the style the element starts from when it
-- first shows, exit the style it eases to as it leaves, and cycle swings it
-- to another style and back while it shows. key keeps a list child's place.
-- An element with a tooltip, item, ability or hoverSound takes the pointer,
-- so the game behind it does not.

-- PanelProps lay out a panel, which groups its children.
export type PanelProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	[number]: Child?,
}

-- LabelProps show text: the label's text children, joined, or the game's
-- text for token in the player's language, with each {s:name} place in it
-- filled from variables.
export type LabelProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	token: string?,
	variables: { [string]: string | number }?,
	[number]: Child?,
}

-- ImageProps show an image from the game's content, such as
-- "file://{images}/hud/icon.png".
export type ImageProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	src: string,
	scaling: Scaling?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- HeroProps show a picture of hero, by the game's hero id.
export type HeroProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	hero: number,
	look: HeroLook?,
	scaling: Scaling?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- AvatarProps show the Steam picture of the player with steamId, a decimal
-- string as Connection.steamId holds it.
export type AvatarProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	steamId: string,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- ButtonProps make a button, which holds its children and calls onPress with
-- the player who pressed it. Players press buttons while they have a cursor.
-- sound is the game's sound event the press plays, such as
-- "UI.MainMenu.Activate", and a disabled button takes no presses.
export type ButtonProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	sound: string?,
	disabled: boolean?,
	onPress: ((player: Player) -> ())?,
	[number]: Child?,
}

-- EntryProps make a box the player types a line into: value is its text,
-- placeholder the faint text it shows empty, and maxLength its limit in
-- characters, 100 unless set, and at most 240 bytes of text. A disabled
-- input takes no changes.
export type EntryProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	disabled: boolean?,
	value: string?,
	placeholder: string?,
	maxLength: number?,
	onChange: ((player: Player, text: string) -> ())?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- SliderProps make a bar the player drags to a number from min to max, 0 to
-- 1 unless set, stopping at each step when step is set. onWheel receives one
-- wheel step and its pointer anchor while enabled.
export type SliderProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	disabled: boolean?,
	value: number?,
	min: number?,
	max: number?,
	step: number?,
	onChange: ((player: Player, value: number) -> ())?,
	onWheel: ((player: Player, wheel: Wheel) -> ())?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- ToggleProps make a box the player checks, labeled by its text children.
export type ToggleProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	disabled: boolean?,
	on: boolean?,
	onChange: ((player: Player, on: boolean) -> ())?,
	[number]: Child?,
}

-- DropdownProps let the player pick one of options; selected is its index,
-- from 0.
export type DropdownProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	hover: Style?,
	tooltip: string?,
	item: string?,
	ability: AbilityTip?,
	hoverSound: string?,
	enter: Style?,
	exit: Style?,
	cycle: Cycle?,
	disabled: boolean?,
	options: { string },
	selected: number?,
	onChange: ((player: Player, index: number) -> ())?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- LayoutProps load a layout from the mod's content addon: src is its path
-- there, such as "shop.xml", and data is the JSON value its script reads
-- with Modlock.data.
export type LayoutProps = {
	key: (string | number)?,
	class: string?,
	style: Style?,
	src: string,
	data: any,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- FragmentProps group children without a panel of their own.
export type FragmentProps = {
	key: (string | number)?,
	[number]: Child?,
}

-- ShownAbility is the ability abilityDisplay shows in one slot.
export type ShownAbility = {
	-- name is the ability's class name, whose window the game shows over it.
	name: string,
	-- image is its picture, a path in the game's content as an image's src.
	image: string,
	-- upgrades holds its bought tiers as Ability.upgrades: bit 0 unlocks it.
	upgrades: number?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- AbilityRow is the four abilities, in slot order, shown for one player.
export type AbilityRow = {
	-- player is whose row it is.
	player: Player,
	-- abilities holds slots 1 to 4; a nil slot shows the game's own.
	abilities: { ShownAbility? },
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- AbilityBarHandler takes a click on the ability in slot, 0 to 3.
export type AbilityBarHandler = (player: Player, slot: number) -> ()

-- Node is one node of the interface tree as the wire carries it.
type Node = { [string]: any }

-- Notice is one toast, shown until the game clock passes expires.
type Notice = {
	-- id numbers the notice, which keys it in the stack.
	id: number,
	-- text is what it says.
	text: string,
	-- expires is when it ends, on the game clock.
	expires: number,
}

-- Cue is one sound the renderer plays once.
type Cue = {
	-- number orders the cue, so the renderer plays each once.
	number: number,
	-- sound is the game's sound event.
	sound: string,
	-- seconds cuts the sound after that long, or 0 to play it whole.
	seconds: number,
}

-- Route is one value the mod sends a player's layouts under a name.
type Route = {
	-- name is the route's name.
	name: string,
	-- data is its value as JSON.
	data: string,
}

-- View is what the mod shows one player: its interface and its notices.
type View = {
	-- player is whom the view is for.
	player: Player,
	-- root is the interface the mod shows.
	root: Child?,
	-- hud lists the parts of the game's HUD to hide.
	hud: { HudPart },
	-- cursor is true while the mod frees the player's mouse cursor.
	cursor: boolean,
	-- asks counts the mod's calls that freed the cursor.
	asks: number,
	-- routes are the routes in the order the mod first set them.
	routes: { Route },
	-- cues are the last sounds played, which the renderer plays once each.
	cues: { Cue },
	-- bar takes clicks on the game's ability bar, while the mod asks for them.
	bar: AbilityBarHandler?,
	-- display is the abilities shown on Tab's rows and the ability bar.
	display: { AbilityRow },
	-- notices are the toasts showing, oldest first.
	notices: { Notice },
}

-- Screen is what one player's interface holds, as the host last accepted it.
type Screen = {
	-- nodes lists the nodes in layout order, children before their parent.
	nodes: { Node },
	-- encoded maps each node's id to its wire encoding, to find what changed.
	encoded: { [string]: string },
	-- ids maps each element's place in the tree to its node's id. A place is
	-- its parent's place, then "/" for the children, ",index" or ",=key" in a
	-- list, and ">" for what a fragment holds.
	ids: { [string]: string },
	-- presses maps each button's id to its handler.
	presses: { [string]: (player: Player) -> () },
	-- changes maps each input's id to its handler.
	changes: { [string]: (player: Player, value: any) -> () },
	-- wheels holds only the enabled sliders that opted in to wheel events.
	wheels: { [string]: (player: Player, wheel: Wheel) -> () },
	-- next numbers the next new node, so an id is never reused.
	next: number,
}

-- Layout holds one screen while place lays it out, with the screen before
-- it whose ids it keeps.
type Layout = {
	-- screen is the screen being laid out.
	screen: Screen,
	-- previous is the screen the player holds, or nil for a whole screen.
	previous: Screen?,
}

-- elementMeta marks the tables the element functions build, which tells an
-- element from a list of children.
local elementMeta = {}

-- views holds what the mod shows each player, by slot.
local views: { [number]: View } = {}

-- screens holds each player's interface by slot.
local screens: { [number]: Screen } = {}

-- generations holds each occupied slot's connection generation from the
-- last player list, to tell a new player in a slot from the one before.
local generations: { [number]: number } = {}

-- sheets lists the mod's style sheets, in the order the mod added them.
local sheets: { string } = {}

-- now is the game clock at the last frame, in seconds.
local now = 0

-- nextNotice numbers the next notice, which keys it in the stack.
local nextNotice = 0

-- nextCue numbers the next sound, so the renderer plays each once.
local nextCue = 1

-- maxCues is how many sounds the root keeps, so a player whose screens come
-- slower than the mod plays sounds misses none of the last few.
local maxCues = 8

-- maxNotices bounds the stack; a newer notice pushes out the oldest.
local maxNotices = 4

-- noticeTransition slides a notice in and fades it out as it expires.
local noticeTransition: Transition = { seconds = 0.25, easing = "out" }

-- aligns maps each alignment to its wire name.
local aligns: { [string]: string } = {
	left = "start",
	top = "start",
	center = "center",
	right = "end",
	bottom = "end",
}

-- easings maps each easing to its wire name.
local easings: { [string]: string } = {
	out = "easeOut",
	["in"] = "easeIn",
	inOut = "easeInOut",
	linear = "linear",
}

-- kinds maps each element that draws as itself to its node's kind; the
-- inputs are all "input".
local kinds: { [string]: string } = {
	panel = "panel",
	label = "label",
	image = "image",
	hero = "heroImage",
	button = "button",
	layout = "layout",
	avatar = "avatar",
	entry = "input",
	slider = "input",
	toggle = "input",
	dropdown = "input",
}

-- element builds an element of type from props.
local function element(kind: string, props: any): Element
	local key = props.key
	return setmetatable({ type = kind, props = props, key = if key ~= nil then tostring(key) else nil }, elementMeta) :: any
end

-- isElement reports whether child is an element rather than a list.
local function isElement(child: any): boolean
	return type(child) == "table" and getmetatable(child) == elementMeta
end

-- panel groups its children.
function ui.panel(props: PanelProps): Element
	return element("panel", props)
end

-- label shows its text children, joined, or the game's text for token.
function ui.label(props: LabelProps): Element
	return element("label", props)
end

-- image shows an image from the game's content.
function ui.image(props: ImageProps): Element
	return element("image", props)
end

-- hero shows a picture of a hero.
function ui.hero(props: HeroProps): Element
	return element("hero", props)
end

-- avatar shows a player's Steam picture.
function ui.avatar(props: AvatarProps): Element
	return element("avatar", props)
end

-- button holds its children and calls onPress when pressed.
function ui.button(props: ButtonProps): Element
	return element("button", props)
end

-- entry is a box the player types a line into.
function ui.entry(props: EntryProps): Element
	return element("entry", props)
end

-- slider is a bar the player drags to a number.
function ui.slider(props: SliderProps): Element
	return element("slider", props)
end

-- toggle is a box the player checks.
function ui.toggle(props: ToggleProps): Element
	return element("toggle", props)
end

-- dropdown lets the player pick one of its options.
function ui.dropdown(props: DropdownProps): Element
	return element("dropdown", props)
end

-- layout loads a layout from the mod's content addon.
function ui.layout(props: LayoutProps): Element
	return element("layout", props)
end

-- fragment groups children without a panel of its own, and keys them as one
-- list child. A component that returns several children returns a fragment.
function ui.fragment(props: FragmentProps): Element
	return element("fragment", props)
end

-- children returns the children in the list part of props as an element's
-- children: nil for none, the child itself for one, and a list for more. A
-- component passes its own children on with it.
function ui.children(props: { [any]: any }): Child?
	local count = table.maxn(props)
	if count == 0 then
		return nil
	elseif count == 1 then
		return props[1]
	end
	local list = table.create(count)
	for i = 1, count do
		list[i] = props[i]
	end
	return list :: any
end

-- itemMultiplier is MurmurHash2's mixing constant.
local itemMultiplier = 0x5bd1e995

-- multiply returns a * b modulo 2^32 for 32-bit a and b, in 16-bit halves so
-- no product loses bits.
local function multiply(a: number, b: number): number
	local low = bit32.band(a, 0xffff) * b
	local high = bit32.band(bit32.rshift(a, 16) * bit32.band(b, 0xffff), 0xffff)
	return bit32.band(low + bit32.lshift(high, 16), 0xffffffff)
end

-- itemId returns the game's id of an item or ability by its class name: the
-- MurmurHash2 of the name with the game's seed.
local function itemId(name: string): number
	-- Seed the hash with the name's length.
	local h = bit32.bxor(0x31415926, #name)
	local i = 1

	-- Mix in every whole four bytes.
	while #name - i + 1 >= 4 do
		local k: any = string.unpack("<I4", name, i)
		k = multiply(k, itemMultiplier)
		k = bit32.bxor(k, bit32.rshift(k, 24))
		k = multiply(k, itemMultiplier)
		h = bit32.bxor(multiply(h, itemMultiplier), k)
		i += 4
	end

	-- Mix in the last one to three bytes, then finish.
	local rest = #name - i + 1
	if rest >= 3 then
		h = bit32.bxor(h, bit32.lshift(string.byte(name, i + 2), 16))
	end
	if rest >= 2 then
		h = bit32.bxor(h, bit32.lshift(string.byte(name, i + 1), 8))
	end
	if rest >= 1 then
		h = multiply(bit32.bxor(h, string.byte(name, i)), itemMultiplier)
	end
	h = multiply(bit32.bxor(h, bit32.rshift(h, 13)), itemMultiplier)
	return bit32.bxor(h, bit32.rshift(h, 15))
end

-- jsonString returns text as a JSON string.
local function jsonString(text: string): string
	return '"'
		.. string.gsub(text, '[%c"\\]', function(c: string): string
			local named = ({ ['"'] = '\\"', ["\\"] = "\\\\", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" })[c]
			return named or string.format("\\u%04x", string.byte(c))
		end)
		.. '"'
end

-- json returns value as JSON, or nil when JSON cannot hold it, such as a
-- function. A table with a first element is an array; any other table is an
-- object with string keys in sorted order.
local function json(value: any): string?
	-- Write the plain values.
	local kind = type(value)
	if kind == "boolean" then
		return tostring(value)
	elseif kind == "number" then
		return if value == value and value ~= math.huge and value ~= -math.huge then tostring(value) else "null"
	elseif kind == "string" then
		return jsonString(value)
	elseif kind ~= "table" then
		return nil
	end

	-- Write an array's items, or an object's members.
	local parts = {}
	if value[1] ~= nil then
		for i = 1, #value do
			local item = json(value[i])
			if item == nil then
				return nil
			end
			table.insert(parts, item)
		end
		return `[{table.concat(parts, ",")}]`
	end
	local names = {}
	for name in value do
		if type(name) ~= "string" then
			return nil
		end
		table.insert(names, name)
	end
	table.sort(names)
	for _, name in names do
		local member = json(value[name])
		if member == nil then
			return nil
		end
		table.insert(parts, `{jsonString(name)}:{member}`)
	end
	return `\{{table.concat(parts, ",")}}`
end

-- color encodes "#rrggbb" or "#rrggbbaa" as 0xRRGGBBAA, or nil for another
-- value.
local function color(value: string?): number?
	if value == nil then
		return nil
	end
	local hex = string.gsub(value, "^#", "")
	if not (string.match(hex, "^%x%x%x%x%x%x$") or string.match(hex, "^%x%x%x%x%x%x%x%x$")) then
		return nil
	end
	return tonumber(if #hex == 6 then hex .. "ff" else hex, 16)
end

-- length encodes a length as its wire case.
local function length(value: Length?): Node?
	if value == nil then
		return nil
	elseif type(value) == "number" then
		return { pixels = value }
	elseif value == "fit" then
		return { fitChildren = true }
	elseif value == "fill" then
		return { fill = 1 }
	end
	return { percent = tonumber(string.match(value, "^%s*([-+]?[%d.]+)")) or 0 / 0 }
end

-- edges encodes edges.
local function edges(value: Edges?): Node?
	if value == nil then
		return nil
	elseif type(value) == "number" then
		return { top = value, right = value, bottom = value, left = value }
	elseif #value == 2 then
		return { top = value[1], right = value[2], bottom = value[1], left = value[2] }
	end
	return { top = value[1], right = value[2], bottom = value[3], left = value[4] }
end

-- gradient encodes a gradient, spacing colors without places evenly.
local function gradient(value: Gradient?): Node?
	if value == nil then
		return nil
	end
	local last = math.max(1, #value.colors - 1)
	local stops = {}
	for i, each in value.colors do
		local at = if value.at then value.at[i] else nil
		table.insert(stops, { at = if at ~= nil then at else (i - 1) / last, color = color(each) })
	end
	return { shape = value.shape, stops = stops }
end

-- shadow encodes a shadow.
local function shadow(value: Shadow?): Node?
	if value == nil then
		return nil
	end
	return {
		x = value.x,
		y = value.y,
		blur = value.blur,
		spread = value.spread,
		color = color(value.color),
		inset = value.inset,
	}
end

-- transform encodes a style's move, scale, turn and origin, or nil for none.
local function transform(style: Style): Node?
	local translate, scale, rotate, origin = style.translate, style.scale, style.rotate, style.origin
	if translate == nil and scale == nil and rotate == nil and origin == nil then
		return nil
	end
	local scales: { number }? = if type(scale) == "number" then { scale, scale } else scale
	return {
		translateX = if translate then translate[1] else nil,
		translateY = if translate then translate[2] else nil,
		scaleX = if scales then scales[1] else nil,
		scaleY = if scales then scales[2] else nil,
		rotate = rotate,
		originX = if origin then origin[1] else nil,
		originY = if origin then origin[2] else nil,
	}
end

-- transition encodes a transition: seconds, or a Transition.
local function transition(value: (number | Transition)?): Node?
	if value == nil then
		return nil
	elseif type(value) == "number" then
		return { seconds = value }
	end
	return {
		seconds = value.seconds,
		delay = value.delay,
		easing = if value.easing then easings[value.easing] else nil,
	}
end

-- wireStyle encodes style.
local function wireStyle(style: Style?): Node?
	if style == nil then
		return nil
	end
	return {
		width = length(style.width),
		height = length(style.height),
		flow = style.flow,
		horizontalAlign = if style.horizontalAlign then aligns[style.horizontalAlign] else nil,
		verticalAlign = if style.verticalAlign then aligns[style.verticalAlign] else nil,
		margin = edges(style.margin),
		padding = edges(style.padding),
		background = color(style.background),
		color = color(style.color),
		fontSize = style.fontSize,
		bold = style.bold,
		textAlign = if style.textAlign then aligns[style.textAlign] else nil,
		borderRadius = style.borderRadius,
		opacity = style.opacity,
		face = style.face,
		uppercase = style.uppercase,
		letterSpacing = style.letterSpacing,
		shadow = style.shadow,
		borderWidth = style.borderWidth,
		borderColor = color(style.borderColor),
		x = length(style.x),
		y = length(style.y),
		minWidth = length(style.minWidth),
		maxWidth = length(style.maxWidth),
		minHeight = length(style.minHeight),
		maxHeight = length(style.maxHeight),
		zIndex = style.zIndex,
		overflowX = style.overflowX or style.overflow,
		overflowY = style.overflowY or style.overflow,
		gradient = gradient(style.gradient),
		boxShadow = shadow(style.boxShadow),
		textShadow = shadow(style.textShadow),
		tint = color(style.tint),
		saturation = style.saturation,
		brightness = style.brightness,
		contrast = style.contrast,
		blur = style.blur,
		italic = style.italic,
		weight = style.weight,
		lineHeight = style.lineHeight,
		noWrap = style.noWrap,
		textOverflow = style.textOverflow,
		decoration = style.decoration,
		transform = transform(style),
		transition = transition(style.transition),
	}
end

-- cycle encodes a cycle.
local function cycle(value: Cycle?): Node?
	if value == nil then
		return nil
	end
	return { style = wireStyle(value.style), seconds = value.seconds, count = value.count }
end

-- classes splits an element's class into its names, or nil for none.
local function classes(names: string?): { string }?
	local list = {}
	for name in string.gmatch(names or "", "[^ ]+") do
		table.insert(list, name)
	end
	return if #list == 0 then nil else list
end

-- variables encodes a localized label's variables as text, in name order.
local function variables(value: { [string]: string | number }?): { Node }?
	if value == nil then
		return nil
	end
	local entries: { Node } = {}
	for name, filled in value do
		table.insert(entries, { key = name, value = tostring(filled) } :: Node)
	end
	table.sort(entries, function(a: Node, b: Node): boolean
		return a.key < b.key
	end)
	return entries
end

-- text joins a label's text children.
local function text(child: any): string
	if type(child) == "string" then
		return child
	elseif type(child) == "number" then
		return tostring(child)
	elseif type(child) ~= "table" or isElement(child) then
		return ""
	end
	local parts = {}
	for i = 1, table.maxn(child) do
		table.insert(parts, text(child[i]))
	end
	return table.concat(parts)
end

-- base36 writes n in base 36, as node ids are.
local function base36(n: number): string
	local digits = "0123456789abcdefghijklmnopqrstuvwxyz"
	local out = ""
	repeat
		local digit = n % 36
		out = string.sub(digits, digit + 1, digit + 1) .. out
		n = n // 36
	until n == 0
	return out
end

-- wireCodec sends changes through the host's Ui call and encodes nodes to
-- compare them, as the library serves them; the handlers it takes are set
-- below.
local wireCodec: modlock.InterfaceWire

-- add records a node in layout's screen.
local function add(layout: Layout, node: Node)
	table.insert(layout.screen.nodes, node)
	layout.screen.encoded[node.id] = wireCodec.encode(node)
end

-- id returns the node id for the element at place, the one it had in the
-- previous screen or else a new one.
local function id(layout: Layout, place: string): string
	local previous = layout.previous
	local kept = if previous then previous.ids[place] else nil
	local found = kept or base36(layout.screen.next)
	if kept == nil then
		layout.screen.next += 1
	end
	layout.screen.ids[place] = found
	return found
end

-- place is declared ahead of placeChildren, which calls it.
local place: (layout: Layout, child: any, at: string, out: { string }) -> ()

-- placeList lays out the first count items of list at place at, each at its
-- index from 0 or at its key.
local function placeList(layout: Layout, list: { [number]: any }, count: number, at: string, out: { string })
	for i = 1, count do
		local item = list[i]
		local key = if isElement(item) then item.key else nil
		place(layout, item, if key ~= nil then `{at},={key}` else `{at},{i - 1}`, out)
	end
end

-- placeChildren lays out the children in the list part of props at place
-- at: one child at the place itself, more as a list.
local function placeChildren(layout: Layout, props: { [any]: any }, at: string, out: { string })
	local count = table.maxn(props)
	if count == 1 then
		place(layout, props[1], at, out)
	else
		placeList(layout, props, count, at, out)
	end
end

-- input lays out an input of kind from props in node.
local function input(layout: Layout, node: Node, kind: string, props: { [any]: any })
	-- A disabled input's change runs nothing, whoever sends it.
	node.disabled = props.disabled
	if props.onChange and not props.disabled then
		layout.screen.changes[node.id] = props.onChange
	end

	-- Each control carries its value and its bounds.
	if kind == "entry" then
		node.textEntry = {
			text = if props.value ~= nil then tostring(props.value) else "",
			placeholder = props.placeholder,
			maxLength = props.maxLength,
		}
	elseif kind == "slider" then
		local wheel = props.onWheel ~= nil and not props.disabled
		if wheel then
			layout.screen.wheels[node.id] = props.onWheel
		end
		node.slider = {
			value = tonumber(props.value) or 0,
			min = props.min,
			max = props.max,
			step = props.step,
			wheel = wheel,
		}
	elseif kind == "toggle" then
		node.text = text(ui.children(props))
		node.toggle = { on = props.on }
	else
		node.dropdown = { options = table.clone(props.options or {}), selected = props.selected }
	end
end

-- place lays out child at place at in layout and appends its nodes' ids to
-- out.
function place(layout: Layout, child: any, at: string, out: { string })
	-- Lay out empty values, lists, text, and the children of fragments.
	if child == nil or type(child) == "boolean" then
		return
	elseif type(child) == "table" and not isElement(child) then
		placeList(layout, child, table.maxn(child), at, out)
		return
	elseif type(child) ~= "table" then
		local node = { id = id(layout, at), kind = "label", text = tostring(child) }
		add(layout, node)
		table.insert(out, node.id)
		return
	elseif child.type == "fragment" then
		placeChildren(layout, child.props, `{at}>`, out)
		return
	end

	-- Draw an element with the props every element takes.
	local props = child.props
	local ability: AbilityTip? = props.ability
	local named = ability ~= nil and ability.name ~= ""
	local node: Node = {
		id = id(layout, at),
		kind = kinds[child.type],
		style = wireStyle(props.style),
		hover = wireStyle(props.hover),
		tooltip = props.tooltip,
		item = if props.item and props.item ~= "" then itemId(props.item) else nil,
		ability = if named and ability then itemId(ability.name) else nil,
		abilityPlayer = if named and ability then ability.player.slot else nil,
		hoverSound = props.hoverSound,
		enter = wireStyle(props.enter),
		exit = wireStyle(props.exit),
		cycle = cycle(props.cycle),
		classes = classes(props.class),
	}

	-- Add each kind's own props.
	local kind = child.type
	if kind == "label" then
		node.text = text(ui.children(props))
		node.token = props.token
		node.variables = variables(props.variables)
	elseif kind == "avatar" then
		node.steamId = props.steamId
	elseif kind == "entry" or kind == "slider" or kind == "toggle" or kind == "dropdown" then
		input(layout, node, kind, props)
	elseif kind == "image" then
		node.image = props.src or ""
		node.scaling = props.scaling
	elseif kind == "hero" then
		node.hero = props.hero
		node.heroLook = props.look
		node.scaling = props.scaling
	elseif kind == "button" then
		-- A disabled button's press runs nothing, whoever sends it.
		node.sound = props.sound
		node.disabled = props.disabled
		if props.onPress and not props.disabled then
			layout.screen.presses[node.id] = props.onPress
		end
		node.children = {}
		placeChildren(layout, props, `{at}/`, node.children)
	elseif kind == "layout" then
		node.layout = props.src or ""
		node.data = if props.data == nil then "" else json(props.data) or ""
	else
		node.kind = "panel"
		node.children = {}
		placeChildren(layout, props, `{at}/`, node.children)
	end
	add(layout, node)
	table.insert(out, node.id)
end

-- displayRows puts rows on the wire for viewer, with each player's name and
-- hero as the Tab screen shows them now.
local function displayRows(viewer: Player, rows: { AbilityRow }): { Node }
	-- Read the names once, and each hero's class from its id.
	if #rows == 0 then
		return {}
	end
	local names: { [number]: string } = {}
	for _, connection in modlock.players() do
		names[connection.player.slot] = connection.name
	end
	local heroNames: { [number]: string } = {}
	for _, hero in modlock.heroes do
		heroNames[hero.id] = hero.name
	end

	-- Write each row's player, name, hero and up to four abilities.
	local out: { Node } = {}
	for _, row in rows do
		local pawn = row.player:pawn()
		local name = names[row.player.slot] or ""
		-- A Steam name is at most 32 characters, within the host's 128 bytes.
		local cut = utf8.offset(name, 33)
		if cut then
			name = string.sub(name, 1, cut - 1)
		end
		local abilities = {}
		for i = 1, math.min(4, table.maxn(row.abilities)) do
			local shown = row.abilities[i]
			table.insert(abilities, {
				ability = if shown and shown.name ~= "" then itemId(shown.name) else 0,
				image = if shown then shown.image else "",
				upgrades = if shown then shown.upgrades or 0 else 0,
			})
		end
		table.insert(out, {
			player = row.player,
			name = name,
			hero = if pawn then heroNames[pawn.hero] or "" else "",
			own = row.player.slot == viewer.slot,
			abilities = abilities,
		} :: Node)
	end
	return out
end

-- notices stacks the notices at the lower left, newest at the bottom, clear
-- of the game's health, weapon and spirit stats and the status icons above
-- them. The stack fades with its last notice.
local function notices(stack: { Notice }): Element
	local labels = {}
	for _, notice in stack do
		table.insert(
			labels,
			ui.label {
				key = notice.id,
				style = {
					horizontalAlign = "left",
					margin = { 4, 0, 0, 0 },
					padding = { 5, 10 },
					background = "#101418d8",
					color = "#f2f2f2",
					fontSize = 16,
					borderRadius = 4,
					transition = noticeTransition,
				},
				enter = { opacity = 0, translate = { -16, 0 } },
				exit = { opacity = 0 },
				notice.text,
			}
		)
	end
	return ui.panel {
		style = {
			width = "fit",
			height = "fit",
			flow = "down",
			horizontalAlign = "left",
			verticalAlign = "bottom",
			margin = { 0, 0, 260, 24 },
			transition = noticeTransition,
		},
		exit = { opacity = 0 },
		labels,
	}
end

-- render lays the view out as nodes, keeping the ids of elements that stayed
-- in place since previous, beside a node for each route.
local function render(previous: Screen?, current: View, display: { Node }): Screen
	-- The root is the full-screen panel the elements hang from, and it lists
	-- the parts of the game's HUD to hide, whether the cursor is free, the
	-- mod's sheets and its last sounds.
	local layout: Layout = {
		previous = previous,
		screen = {
			nodes = {},
			encoded = {},
			ids = {},
			presses = {},
			changes = {},
			wheels = {},
			next = if previous then previous.next else 0,
		},
	}
	local children = {}
	local root = { current.root or false, #current.notices ~= 0 and notices(current.notices) }
	placeList(layout, root, 2, "/", children)
	add(layout, {
		id = "",
		kind = "panel",
		children = children,
		hideHud = table.clone(current.hud),
		freeCursor = current.cursor,
		freeCursorAsks = current.asks,
		sheets = table.clone(sheets),
		cues = table.clone(current.cues),
		abilityBarPresses = current.bar ~= nil,
		abilityDisplay = display,
	})

	-- A route's id holds a colon, which no element's id does.
	for _, route in current.routes do
		add(layout, { id = `route:{route.name}`, kind = "route", route = route.name, data = route.data })
	end
	return layout.screen
end

-- diff returns the change from previous to next, or nil when nothing
-- changed.
local function diff(previous: Screen?, next: Screen): Node?
	local set = {}
	for _, node in next.nodes do
		if previous == nil or previous.encoded[node.id] ~= next.encoded[node.id] then
			table.insert(set, node)
		end
	end
	local removed = {}
	if previous then
		for _, node in previous.nodes do
			if next.encoded[node.id] == nil then
				table.insert(removed, node.id)
			end
		end
	end
	if previous and #set == 0 and #removed == 0 then
		return nil
	end
	return { reset = previous == nil, set = set, removed = removed }
end

-- view returns what the mod shows player, made empty the first time.
local function view(player: Player): View
	local found = views[player.slot]
	if found then
		return found
	end
	local made: View = {
		player = player,
		hud = {},
		cursor = false,
		asks = 0,
		routes = {},
		cues = {},
		display = {},
		notices = {},
	}
	views[player.slot] = made
	return made
end

-- draw sends what changed in player's interface and notices.
local function draw(player: Player): boolean
	-- Render the player's view and any notices against the last screen sent.
	local current = view(player)
	local previous = screens[player.slot]
	local display = displayRows(player, current.display)
	local next = render(previous, current, display)
	local change = diff(previous, next)

	-- Send the change; a failed send means the player may have left, so send
	-- the screen whole.
	if change and not wireCodec.send(player, change) then
		local whole = render(nil, current, display)
		if not wireCodec.send(player, diff(nil, whole)) then
			screens[player.slot] = nil
			return false
		end
		screens[player.slot] = whole
		return true
	end

	-- Keep the screen the player now holds.
	screens[player.slot] = next
	return true
end

-- show replaces the interface the mod shows player with root. A player who
-- left and a mod that restarted show nothing until the next call.
function ui.show(player: Player, root: Child?): boolean
	view(player).root = root
	return draw(player)
end

-- hideHud hides parts of the game's own HUD from player until the next call;
-- an empty list shows them all again.
function ui.hideHud(player: Player, parts: { HudPart }): boolean
	view(player).hud = table.clone(parts)
	return draw(player)
end

-- freeCursor shows player's mouse cursor without Tab held while free is
-- true, so they can click the mod's panels, such as a pick screen; false
-- gives the mouse back to aiming. Each call with true shows the cursor again,
-- even when the player put it away with the game's own keys; a screen that
-- changes without a call leaves their choice be. The game does not stop the
-- player's shots, camera or movement while the cursor is up: a screen that
-- must not fire a weapon calls player:blockInput for the buttons it takes.
function ui.freeCursor(player: Player, free: boolean): boolean
	local current = view(player)
	current.cursor = free
	if free then
		current.asks += 1
	end
	return draw(player)
end

-- abilityBarPresses calls onPress with the slot, 0 to 3, each time player
-- clicks the icon or upgrade pips of an ability on the game's own ability
-- bar, as the game does when it trains one with Tab held; without onPress
-- the clicks stop reaching the mod. The game answers a click from the hero's
-- own abilities, and sends nothing for an ability replaceAbility gave, so a
-- mod that replaces abilities answers here, with trainAbility. A click the
-- game answers itself, with upgrade_ability, reaches the mod both ways.
-- Clicks reach the mod only while its screen holds the player's HUD: a
-- player who left, or whose session ended, sends none.
function ui.abilityBarPresses(player: Player, onPress: AbilityBarHandler?): boolean
	view(player).bar = onPress
	return draw(player)
end

-- abilityDisplay shows viewer each row's abilities in place of the ones the
-- game shows for that player's hero: on the player's row of the game's Tab
-- screen, and for viewer's own row on viewer's ability bar too, in the
-- game's frame, ring and pips, with the game's ability window over each. The
-- game draws these from the hero's standard abilities, so one replaceAbility
-- gave shows as the ability it replaced. Without rows the game's own come
-- back, as they do when viewer leaves or the session ends. Rows match the Tab
-- screen's by the player's name and else by the hero's, which a bot's row
-- shows, so each row's player should have a hero.
function ui.abilityDisplay(viewer: Player, rows: { AbilityRow }?): boolean
	local shown = {}
	for i, row in rows or {} do
		if i > 64 then
			break
		end
		table.insert(shown, row)
	end
	view(viewer).display = shown
	return draw(viewer)
end

-- cursorFree reports whether player's mouse cursor is up now, whether the
-- mod freed it with freeCursor or the player did with the game's own keys.
-- It is nil while the player has no hero to read.
function ui.cursorFree(player: Player): boolean?
	local pawn = player:pawn()
	if pawn == nil then
		return nil
	end
	local free = modlock.readField(pawn.entity, "CCitadelPlayer_MovementServices", "m_bHasFreeCursor", "bool")
	return if type(free) == "boolean" then free else nil
end

-- sheet adds the style sheet at path in the mod's content styles directory,
-- such as "hud.css", to every interface the mod shows, after the sheets
-- added before it. Adding a sheet again changes nothing. A path is letters,
-- digits, "_", "-" and "." in segments separated by "/", ending in ".css";
-- another adds nothing and returns false.
function ui.sheet(path: string): boolean
	-- Refuse a path the renderer cannot load, and keep a sheet added before.
	if string.sub(path, -4) ~= ".css" then
		return false
	end
	for _, part in string.split(path, "/") do
		if not string.match(part, "^[%w_%.%-]+$") or part == ".." then
			return false
		end
	end
	if table.find(sheets, path) then
		return true
	end

	-- Redraw every interface with the new sheet.
	table.insert(sheets, path)
	for _, current in views do
		draw(current.player)
	end
	return true
end

-- route sends player value under name, for every layout of the mod to read
-- with Modlock.route, until the next call; nil removes it. A name is
-- letters, digits, "_", "-" and ".", and the value is anything JSON holds. A
-- route with another name, or a value JSON cannot hold, sends nothing and
-- returns false.
function ui.route(player: Player, name: string, value: any): boolean
	-- Refuse a name no attribute can carry and a value JSON cannot hold.
	local encoded = if value == nil then nil else json(value)
	if not string.match(name, "^[%w_%.%-]+$") or (value ~= nil and encoded == nil) then
		return false
	end

	-- Store the value in its place, or forget the route, and redraw.
	local routes = view(player).routes
	local at = nil
	for i, route in routes do
		if route.name == name then
			at = i
		end
	end
	if encoded == nil then
		if at then
			table.remove(routes, at)
		end
		return draw(player)
	end
	if at then
		routes[at].data = encoded :: string
	else
		table.insert(routes, { name = name, data = encoded :: string })
	end
	return draw(player)
end

-- hide removes the interface, the routes and the notices the mod shows
-- player.
function ui.hide(player: Player): boolean
	views[player.slot] = nil
	screens[player.slot] = nil
	return wireCodec.send(player, { reset = true })
end

-- toast shows player text in a notice at the lower left for seconds, 4
-- unless set. Notices stack, newest last, slide in, and fade as they expire.
function ui.toast(player: Player, text: string, seconds: number?): boolean
	local current = view(player)
	table.insert(current.notices, { id = nextNotice, text = text, expires = now + (seconds or 4) })
	nextNotice += 1
	if #current.notices > maxNotices then
		table.remove(current.notices, 1)
	end
	return draw(player)
end

-- uiSound plays the game's sound event sound, such as "UI.ItemDraft.Start",
-- in player's game alone, once; seconds cuts it after that long, keeping
-- only its start. It plays with the next screen the player reads, and not at
-- all if the player reads it only after the mod's next few sounds.
function ui.uiSound(player: Player, sound: string, options: { seconds: number? }?): boolean
	local current = view(player)
	table.insert(current.cues, { number = nextCue, sound = sound, seconds = if options then options.seconds or 0 else 0 })
	nextCue += 1
	if #current.cues > maxCues then
		table.remove(current.cues, 1)
	end
	return draw(player)
end

-- press runs the handler of the button the player pressed.
local function press(player: Player, node: string)
	-- A click on the game's ability bar names its slot, which no button's id
	-- does.
	local slot = string.match(node, "^ability:([0-3])$")
	if slot then
		local current = views[player.slot]
		if current and current.bar then
			current.bar(player, tonumber(slot) :: number)
		end
		return
	end
	local screen = screens[player.slot]
	local handler = if screen then screen.presses[node] else nil
	if handler then
		handler(player)
	end
end

-- change runs the handler of the input the player changed, with its value as
-- the host checked it; a wheel step goes to the slider's onWheel.
local function change(player: Player, node: string, value: (string | number | boolean | Wheel)?)
	local screen = screens[player.slot]
	if screen == nil then
		return
	end
	if type(value) == "table" then
		local handler = screen.wheels[node]
		if handler then
			handler(player, value)
		end
		return
	end
	local handler = screen.changes[node]
	if handler then
		handler(player, value)
	end
end

-- tick advances the clock to the frame at seconds and fades expiring
-- notices.
local function tick(seconds: number)
	now = seconds
	for _, current in views do
		if #current.notices == 0 then
			continue
		end
		local kept = {}
		for _, notice in current.notices do
			if notice.expires > now then
				table.insert(kept, notice)
			end
		end
		current.notices = kept
		draw(current.player)
	end
end

-- forget drops the view and screen of each slot whose player left, or whose
-- connection a new player took, so a new player starts from nothing.
local function forget(players: { modlock.Connection })
	local present: { [number]: number } = {}
	for _, connection in players do
		present[connection.player.slot] = connection.generation
	end
	for slot, generation in generations do
		if present[slot] ~= generation then
			views[slot] = nil
			screens[slot] = nil
		end
	end
	generations = present
end

-- Take the interface's events from the library, which loads without this
-- module.
wireCodec = modlock.serveInterface({ press = press, change = change, tick = tick, players = forget })

return ui
