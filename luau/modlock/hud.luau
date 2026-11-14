--!strict
-- hud is the HUD kit: components that draw a mod's HUD in the stock HUD's
-- look, from the tokens in @modlock/theme. Text sets VALVEPulp capitals over
-- a soft shadow, plates and pills hold text over a bright sky, and pips count
-- as the stock round counter does. The roster lists the players down the top
-- right corner. Actions are the game's menu buttons, with its hover and press
-- sounds, and a meter fills toward its value.
--
--   ui.show(player, hud.TopStrip { heading = "Round 2", hud.Score { points = { 1, 0 } } })

local modlock = require("@modlock")
local theme = require("@modlock/theme")
local ui = require("@modlock/ui")

-- The kit's types, from the modules it builds on.
type Player = modlock.Player
type Child = ui.Child
type Element = ui.Element
type Style = ui.Style
type Tone = theme.Tone
type TextRole = theme.TextRole
type PipShape = theme.PipShape

local hud = {}

-- tones, types and pipShapes read the theme by name.
local tones: { [string]: string } = theme.tones :: any
local types: { [string]: theme.Type } = theme.types :: any
local pipShapes: { [string]: theme.PipSize } = theme.pipShapes :: any

-- Fields is a table the kit merges and copies field by field.
type Fields = { [any]: any }

-- merge returns a new style with the fields of each style in turn, so a later
-- style overrides an earlier one.
local function merge(...: Style?): Style
	local merged: Fields = {}
	for i = 1, select("#", ...) do
		local style: Fields? = select(i, ...) :: any
		for name, value in style or {} do
			merged[name] = value
		end
	end
	return merged :: any
end

-- element builds a panel or label for the kit's components, holding children
-- as one child, the way they were passed.
local function element(kind: "panel" | "label", style: Style, children: any, key: string?): Element
	local props: Fields = { style = style, key = key }
	props[1] = children
	return if kind == "panel" then ui.panel(props :: any) else ui.label(props :: any)
end

-- builders build each element the kit rebuilds with new props.
local builders: { [string]: (props: any) -> Element } = {
	panel = ui.panel,
	label = ui.label,
	button = ui.button,
}

-- arriving eases a component in from slightly smaller and fades it out as it
-- goes, over a quarter second.
local function arriving(component: Element, from: Style): Element
	local props = table.clone(component.props)
	props.style = merge({ transition = { seconds = 0.25, easing = "out" } }, props.style)
	props.enter = merge({ opacity = 0 }, from)
	props.exit = { opacity = 0 }
	return builders[component.type](props)
end

-- component wraps what a kit component draws as a component of its own, in
-- the place a TypeScript component takes, so both draw the same nodes.
local function component(drawn: Child?, key: (string | number)?): Element
	return ui.fragment { key = key, drawn }
end

-- TextProps set one label in a type role and tone; the list part holds its
-- text.
export type TextProps = {
	-- key keeps the text's place in a list.
	key: (string | number)?,
	-- role is the text's step of the type scale.
	role: TextRole,
	-- tone colors the text; a label is muted and the rest are text.
	tone: Tone?,
	-- plated marks text on a plate or pill, which takes no shadow: on the dark
	-- plate a shadow only smears the glyphs.
	plated: boolean?,
	-- style overrides the role's style.
	style: Style?,
	[number]: Child?,
}

-- text draws Text, centered unless uncentered leaves the alignment to the
-- parent.
local function text(props: TextProps, uncentered: boolean?): Element
	local type = types[props.role]
	local style: Fields = merge({
		face = type.face,
		fontSize = type.size,
		uppercase = type.uppercase,
		shadow = not props.plated,
		color = tones[if props.tone then props.tone elseif props.role == "label" then "muted" else "text"],
		horizontalAlign = "center",
		textAlign = "center",
	}, props.style) :: any
	if uncentered then
		style.horizontalAlign = nil
	end
	return element("label", style :: any, ui.children(props))
end

-- Text is one label in a type role, over the soft shadow unless plated.
function hud.Text(props: TextProps): Element
	return component(text(props), props.key)
end

-- BoxProps lay out a plate's or pill's children, in the list part.
export type BoxProps = {
	-- key keeps the box's place in a list.
	key: (string | number)?,
	-- flow lays the children out down or right.
	flow: ("down" | "right")?,
	-- style overrides the box's style.
	style: Style?,
	[number]: Child?,
}

-- pill draws Pill.
local function pill(props: BoxProps): Element
	return element(
		"panel",
		merge({
			flow = props.flow or "right",
			horizontalAlign = "center",
			background = theme.plate.color,
			borderRadius = 50,
			borderWidth = 2,
			borderColor = theme.pillEdge,
			padding = { 3, 6 },
		}, props.style),
		ui.children(props)
	)
end

-- Plate is the dark plate under HUD text, readable over a bright sky.
function hud.Plate(props: BoxProps): Element
	return component(
		element(
			"panel",
			merge({
				flow = props.flow or "down",
				horizontalAlign = "center",
				background = theme.plate.color,
				borderRadius = theme.plate.radius,
				padding = theme.plate.padding,
			}, props.style),
			ui.children(props)
		),
		props.key
	)
end

-- Pill is the plate rounded into a capsule with a faint parchment edge,
-- flowing right: the counter under a top strip's heading.
function hud.Pill(props: BoxProps): Element
	return component(pill(props), props.key)
end

-- PipsProps show a count out of a fixed total.
export type PipsProps = {
	-- key keeps the pips' place in a list.
	key: (string | number)?,
	-- filled is how many pips are lit.
	filled: number,
	-- total is how many pips show.
	total: number,
	-- tone colors the lit pips.
	tone: Tone,
	-- shape is a round point, as the stock round counter, or a narrow tick.
	shape: PipShape?,
	-- mirror fills from the right, for the far side of a counter.
	mirror: boolean?,
	-- style overrides the row's style.
	style: Style?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Pips shows a count as filled and empty pips, as the stock round counter.
function hud.Pips(props: PipsProps): Element
	local shape = pipShapes[props.shape or "round"]
	local pips = {}
	for index = 0, props.total - 1 do
		local lit = if props.mirror then index >= props.total - props.filled else index < props.filled
		table.insert(
			pips,
			element("panel", {
				width = shape.width,
				height = shape.height,
				borderRadius = shape.radius,
				margin = { 0, 2 },
				background = if lit then tones[props.tone] else theme.emptyPip,
				borderWidth = 2,
				borderColor = if lit then theme.pipRings.filled else theme.pipRings.empty,
			}, nil, `pip-{index}`)
		)
	end
	return component(element("panel", merge({ flow = "right", verticalAlign = "center" }, props.style), pips), props.key)
end

-- TagProps name a status or a result in small capitals; the list part holds
-- its text.
export type TagProps = {
	-- key keeps the tag's place in a list.
	key: (string | number)?,
	-- tone colors the tag.
	tone: Tone,
	-- fill outlines a status read in a list, or fills a result.
	fill: ("outline" | "filled")?,
	-- style overrides the tag's style.
	style: Style?,
	[number]: Child?,
}

-- Tag is small heading capitals in a 1px border of its tone.
function hud.Tag(props: TagProps): Element
	local filled = props.fill == "filled"
	return component(
		element(
			"label",
			merge({
				face = "block",
				fontSize = 12,
				letterSpacing = 1,
				uppercase = true,
				color = if filled then theme.tones.onAccent else tones[props.tone],
				background = if filled then tones[props.tone] else nil,
				borderWidth = 1,
				borderColor = tones[props.tone],
				borderRadius = theme.sizes.radius,
				padding = { 4, 8 },
				verticalAlign = "center",
			}, props.style),
			ui.children(props)
		),
		props.key
	)
end

-- BannerProps announce a moment: a title and a detail under it.
export type BannerProps = {
	-- key keeps the banner's place in a list.
	key: (string | number)?,
	-- title is the moment, in the heading face.
	title: string,
	-- detail is a muted line under it.
	detail: string?,
	-- tone colors the title.
	tone: Tone?,
	-- style overrides the banner's style.
	style: Style?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Banner is a centered title in the heading face with a muted detail. It
-- grows in as it appears and fades as it goes.
function hud.Banner(props: BannerProps): Element
	local banner = element("panel", merge({ flow = "down", horizontalAlign = "center", margin = { 20, 0, 0, 0 } }, props.style), {
		text { role = "title", tone = props.tone, props.title },
		if props.detail then text { role = "caption", tone = "muted", props.detail } else false,
	})
	return component(arriving(banner, { scale = 0.92 }), props.key)
end

-- TopStripProps fill the top block of an arena HUD; the list part holds the
-- counter's content, such as Score and Pips.
export type TopStripProps = {
	-- key keeps the strip's place in a list.
	key: (string | number)?,
	-- heading names the mode or round and the lane, over the counter.
	heading: string?,
	-- line is a short line under the counter, such as a clock or the news.
	line: string?,
	-- lineTone colors the line.
	lineTone: Tone?,
	-- keepTeamBar seats the strip under the stock team bar, which keeps its
	-- clock, heroes and player slots; unset, it takes the bar's place.
	keepTeamBar: boolean?,
	[number]: Child?,
}

-- column centers its children from the top down, top pixels from the top.
local function column(top: number, children: any): Element
	return element("panel", { flow = "down", horizontalAlign = "center", margin = { top, 0, 0, 0 } }, children)
end

-- TopStrip is the top block of an arena HUD: a heading in spaced capitals,
-- the counter pill, and a line under it. Under the kept team bar, the heading
-- and pill fit above the stock objective bar and the line goes below that
-- bar.
function hud.TopStrip(props: TopStripProps): Element
	-- The heading sits over the counter pill, and the line reads under both.
	local heading: Child = if props.heading
		then text { role = "label", style = { letterSpacing = 1 }, props.heading }
		else false
	local counter = pill { style = { margin = { 4, 0, 0, 0 } }, ui.children(props) }
	local line: Child = if props.line
		then text {
			role = "label",
			tone = props.lineTone or "text",
			style = { margin = { 4, 0, 0, 0 }, fontSize = 18, letterSpacing = 1 },
			props.line,
		}
		else false

	-- A column centers its parts from top down. In the bar's place, one column
	-- holds them all; under the kept bar, the line has its own below the
	-- objective bar.
	if not props.keepTeamBar then
		return component(column(14, { heading, counter, line }), props.key)
	end
	return component(
		element("panel", { width = "fill", height = "fill" }, {
			column(theme.teamBarBottom, { heading, counter }),
			if line then column(theme.objectiveBarBottom, line) else false,
		}),
		props.key
	)
end

-- ScoreProps are each side's points, the near side first.
export type ScoreProps = {
	-- key keeps the score's place in a list.
	key: (string | number)?,
	-- points are the near and far side's points.
	points: { number },
	-- tones color each side; unset, the near side is the accent.
	tones: { Tone }?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Score is a counter's points, each side in its tone, in the title face. It
-- sits in a pill, so its text is plated.
function hud.Score(props: ScoreProps): Element
	local sides: { Tone } = props.tones or { "accent", "text" }
	local function side(points: number, tone: Tone): Element
		return text({ role = "title", tone = tone, plated = true, style = { margin = { 0, 10 } }, tostring(points) }, true)
	end
	return component(
		element("panel", { flow = "right", verticalAlign = "center" }, {
			side(props.points[1], sides[1]),
			text({ role = "title", tone = "muted", plated = true, ":" }, true),
			side(props.points[2], sides[2]),
		}),
		props.key
	)
end

-- RosterRow is one player as the roster shows them.
export type RosterRow = {
	-- name is the player's name.
	name: string,
	-- self marks the viewing player's own row.
	self: boolean?,
	-- hero is the player's hero as Pawn.hero gives it, or 0 without one.
	hero: number,
	-- activity is the line under the name, such as "Free roam".
	activity: string,
	-- steamId badges the portrait's corner with the player's Steam avatar, a
	-- decimal string as Connection.steamId holds it.
	steamId: string?,
	-- onPress makes the row a button the viewer presses while they have a
	-- cursor, such as to open a card about that player.
	onPress: ((player: Player) -> ())?,
	-- selected edges the portrait in the accent, as for the row a card shows.
	selected: boolean?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- rosterRows is the most players the roster shows, above the minimap.
hud.rosterRows = 6

-- rosterName is the most characters a roster name shows.
local rosterName = 24

-- portraitSize is the portrait square's side, its 1px edge included.
local portraitSize = 44

-- badgeSize is the side of the avatar badge on a portrait's corner.
local badgeSize = 18

-- portraits maps each hero's id to its small portrait.
local portraits: { [number]: string } = {}
for _, hero in modlock.heroes do
	portraits[hero.id] = hero.portrait
end

-- clip shortens text to at most width characters, at a word break where one
-- falls in the cut, and marks the cut with an ellipsis.
local function clip(line: string, width: number): string
	local count = utf8.len(line)
	if count == nil or count <= width then
		return line
	end
	local cut = string.sub(line, 1, (utf8.offset(line, width) :: number) - 1)
	local space = string.find(cut, " [^ ]*$")
	if space and space > 1 then
		cut = string.sub(cut, 1, space - 1)
	end
	return `{string.match(cut, "^(.-)%s*$")}…`
end

-- rosterKey is what the roster shows, to redraw only when it changes.
function hud.rosterKey(rows: { RosterRow }): string
	local lines = {}
	for i = 1, math.min(#rows, hud.rosterRows) do
		local row = rows[i]
		local marks = (if row.self then "*" else "") .. (if row.selected then "+" else "") .. (if row.onPress then ">" else "")
		table.insert(lines, `{marks}{row.name}\t{row.hero}\t{row.activity}\t{row.steamId or ""}`)
	end
	return table.concat(lines, "\n")
end

-- rosterRow is one player: name and activity, then the portrait, whose edge
-- reads in the accent on the viewing player's own row.
local function rosterRow(row: RosterRow, index: number): Element
	-- The name and its line align right, against the portrait.
	local names = element("panel", { flow = "down", verticalAlign = "center", margin = { 0, 12, 0, 0 } }, {
		text {
			role = "label",
			tone = "text",
			style = { horizontalAlign = "right", textAlign = "right" },
			clip(string.upper(row.name), rosterName),
		},
		text {
			role = "caption",
			tone = "muted",
			style = { horizontalAlign = "right", textAlign = "right", margin = { 1, 0, 0, 0 } },
			row.activity,
		},
	})

	-- The portrait sits in a 1px edge on a raised square, empty without a
	-- hero, with the player's avatar on its lower left corner.
	local src = portraits[row.hero]
	local image: Child = if src then ui.image { src = src, style = { width = "100%", height = "100%" } } else false
	local badge: Child = if row.steamId
		then ui.avatar {
			steamId = row.steamId,
			style = {
				width = badgeSize,
				height = badgeSize,
				horizontalAlign = "left",
				verticalAlign = "bottom",
				borderRadius = badgeSize / 2,
				borderWidth = 1,
				borderColor = theme.shades.dim,
			},
		}
		else false
	local portrait = element(
		"panel",
		{
			width = portraitSize,
			height = portraitSize,
			padding = 1,
			verticalAlign = "center",
			background = if row.self or row.selected then theme.tones.accent else theme.shades.dim,
		},
		element("panel", { width = "100%", height = "100%", background = theme.shades.raised }, { image, badge })
	)

	-- A player who joins slides in from the edge. A pressable row is a button
	-- whose ground lifts under the pointer.
	local style: Style = { flow = "right", horizontalAlign = "right", margin = { 0, 0, 8, 0 } }
	local line = if row.onPress
		then ui.button {
			key = `row-{index}`,
			style = merge(style, { borderRadius = theme.sizes.radius, transition = theme.motion.hover }),
			hover = { background = theme.shades.raised },
			hoverSound = theme.sounds.hover,
			sound = theme.sounds.press,
			onPress = row.onPress,
			{ names, portrait },
		}
		else element("panel", style, { names, portrait }, `row-{index}`)
	return arriving(line, { translate = { 16, 0 } })
end

-- RosterProps list the players and where the roster starts.
export type RosterProps = {
	-- key keeps the roster's place in a list.
	key: (string | number)?,
	-- rows are the players, in order.
	rows: { RosterRow },
	-- top is the roster's top margin, under the FPS readout unless set.
	top: number?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Roster stacks up to rosterRows players down the top right corner, each
-- one's hero portrait at the edge with their name and a line, such as what
-- they are doing or their score, to its left. It shows nothing without rows.
-- A mode that hides the stock scoreboard's heroes shows who else is here
-- with it.
function hud.Roster(props: RosterProps): Element
	if #props.rows == 0 then
		return component(nil, props.key)
	end
	local rows = {}
	for i = 1, math.min(#props.rows, hud.rosterRows) do
		table.insert(rows, rosterRow(props.rows[i], i - 1))
	end
	return component(
		element("panel", {
			width = "fit",
			height = "fit",
			flow = "down",
			horizontalAlign = "right",
			verticalAlign = "top",
			margin = { props.top or theme.fpsReadoutBottom, 24, 0, 0 },
		}, rows),
		props.key
	)
end

-- ActionProps make a button in the game's menu look; the list part holds its
-- text.
export type ActionProps = {
	-- key keeps the action's place in a list.
	key: (string | number)?,
	-- kind is "primary" for the one action a screen leads with, "secondary"
	-- for the rest, and "quiet" for an action that should not draw the eye.
	kind: ("primary" | "secondary" | "quiet")?,
	-- disabled dims the action and takes no presses.
	disabled: boolean?,
	-- onPress takes the press.
	onPress: ((player: Player) -> ())?,
	-- style overrides the button's style.
	style: Style?,
	[number]: Child?,
}

-- ActionLook is one kind of action's fill, edge and text, idle and hovered.
type ActionLook = {
	-- idle is the look at rest.
	idle: Style,
	-- hover is the look under the pointer.
	hover: Style,
	-- text colors the label.
	text: string,
}

-- actionLooks are each kind of action's look.
local actionLooks: { [string]: ActionLook } = {
	primary = {
		idle = { background = theme.tones.text },
		hover = { background = theme.shades.white },
		text = theme.shades.ink,
	},
	secondary = {
		idle = { background = theme.shades.raised, borderWidth = 1, borderColor = theme.shades.faint },
		hover = { background = theme.shades.lit, borderColor = theme.tones.text },
		text = theme.tones.text,
	},
	quiet = {
		idle = { background = theme.shades.clear },
		hover = { background = theme.shades.raised },
		text = theme.shades.quiet,
	},
}

-- Action is a button in the game's menu look: capitals on a fill that
-- lights as the pointer enters, with the main menu's hover and press sounds.
function hud.Action(props: ActionProps): Element
	local look = actionLooks[props.kind or "secondary"]
	local label = element("label", {
		face = "block",
		fontSize = 16,
		uppercase = true,
		letterSpacing = 1,
		color = look.text,
		horizontalAlign = "center",
		verticalAlign = "center",
		noWrap = true,
	}, ui.children(props))
	return component(
		ui.button {
			style = merge(
				{ height = theme.sizes.control, padding = { 0, 14 }, borderRadius = theme.sizes.radius, transition = theme.motion.hover },
				look.idle,
				if props.disabled then { opacity = 0.4 } else nil,
				props.style
			),
			hover = if props.disabled then nil else look.hover,
			hoverSound = if props.disabled then nil else theme.sounds.hover,
			sound = theme.sounds.press,
			disabled = props.disabled,
			onPress = props.onPress,
			label,
		},
		props.key
	)
end

-- MeterProps show how full something is.
export type MeterProps = {
	-- key keeps the meter's place in a list.
	key: (string | number)?,
	-- value is how full the meter is, from 0 to 1.
	value: number,
	-- tone colors the fill.
	tone: Tone?,
	-- width and height size the meter, in pixels; 160 by 8 unless set.
	width: number?,
	height: number?,
	-- style overrides the meter's style.
	style: Style?,
	-- A key of another name is a mistake, which the indexer makes the checker
	-- report.
	[number]: never,
}

-- Meter is a bar that fills from the left toward its value, easing to each
-- new value: a charge, a capture or a timer.
function hud.Meter(props: MeterProps): Element
	local height = props.height or 8
	local value = math.clamp(props.value, 0, 1)
	return component(
		element(
			"panel",
			merge({
				width = props.width or 160,
				height = height,
				background = theme.shades.raised,
				borderRadius = height / 2,
				overflow = "clip",
			}, props.style),
			-- The fill scales rather than resizes, so it eases without layout
			-- work.
			element("panel", {
				width = "100%",
				height = "100%",
				background = tones[if props.tone then props.tone else "accent"],
				scale = { value, 1 },
				origin = { 0, 50 },
				transition = theme.motion.meter,
			})
		),
		props.key
	)
end

return hud
