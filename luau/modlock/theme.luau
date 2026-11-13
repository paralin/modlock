--!strict
-- theme holds the HUD kit's tokens, each named once: the colors, type scale
-- and sizes the kit's components draw with, and that a mod's own panels can
-- share. They follow the stock HUD's look: VALVEPulp capitals and Retail Demo
-- text in off-white with a dark shadow, team colors and translucent black
-- plates.
--
--   local theme = require("@modlock/theme")
--
--   ui.label { style = { color = theme.tones.gold }, "Victory" }

local theme = {}

-- Tone names the colors a surface may use for meaning.
export type Tone = "text" | "muted" | "accent" | "onAccent" | "amber" | "sapphire" | "gas" | "success" | "danger" | "gold"

-- tones maps each meaning to one color. Text, team and danger colors are the
-- stock HUD's offWhite, team1Color, team2Color and colorEnemy.
theme.tones = {
	-- text is the HUD's off-white.
	text = "#ffefd7",
	-- muted is secondary text.
	muted = "#bfb3a1",
	-- accent marks the player's own choices.
	accent = "#fd736d",
	-- onAccent is text on an accent fill.
	onAccent = "#070709",
	-- amber is the Hidden King's team.
	amber = "#d4860b",
	-- sapphire is the Archmother's team.
	sapphire = "#4d75c3",
	-- gas is the gas of a closing arena.
	gas = "#3ee08f",
	-- success is done, under par, ready and live: a step darker than gas, so
	-- status text and tags sit calmer on the plates.
	success = "#36c47f",
	-- danger is the enemy and every warning.
	danger = "#ff410d",
	-- gold is a win or a reward.
	gold = "#ebc934",
}

-- teamTones colors each game team, by its team number: 2 is the Hidden King's
-- amber, 3 the Archmother's sapphire.
theme.teamTones = {
	-- 2 is the Hidden King.
	[2] = "amber" :: Tone,
	-- 3 is the Archmother.
	[3] = "sapphire" :: Tone,
}

-- shades are the neutrals a full screen draws with, from the stock main
-- menu's base styles, so a full screen reads as part of the game's own menus.
-- Fills are opaque offWhite tints of the offBlack backdrop: Panorama blends
-- translucent fills far stronger than their alpha, which let the stock menu
-- show through.
theme.shades = {
	-- quiet is offWhite at about half strength: idle navigation and secondary
	-- text.
	quiet = "#ffefd78c",
	-- faint is offWhite at the popup button's 44%: idle pill borders.
	faint = "#ffefd770",
	-- dim is offWhite at about 40%: counts, placeholders and offline players.
	dim = "#6b675d",
	-- hairline is offWhite at 13%: hairlines between sections and plate edges.
	hairline = "#2b2a24",
	-- ink is offBlack: text on light fills, and the backdrop's hue.
	ink = "#10130d",
	-- backdrop is the menu backdrop: an opaque offBlack that hides the stock
	-- menu.
	backdrop = "#0b0d09",
	-- veil is the backdrop at 95%, under a panel the player reads over the
	-- game.
	veil = "#0b0d09f2",
	-- plate is offWhite at 4%: a plate on the backdrop.
	plate = "#151611",
	-- raised is offWhite at 8%: a row or stat inside a plate.
	raised = "#1e1f19",
	-- lit is offWhite at 13%: a hovered or joined row.
	lit = "#2b2a24",
	-- white is the full-strength fill of a lit primary button.
	white = "#ffffff",
	-- clear is no fill or border, where a state turns one off.
	clear = "#00000000",
}

-- sizes holds the shared sizes: panel buttons and fields share one height,
-- row buttons a shorter one, and every control and tag one corner radius.
theme.sizes = {
	-- control is the height of panel buttons and fields.
	control = 32,
	-- rowControl is the height of a button inside a list row.
	rowControl = 28,
	-- row is the height of one list row.
	row = 32,
	-- radius rounds buttons, fields and tags.
	radius = 4,
	-- panelRadius rounds panels.
	panelRadius = 8,
}

-- Face is one of the game's typefaces: body is Retail Demo, its text; block
-- is VALVEPulp, set in capitals for the stock clock, round titles and
-- callouts; serif is Reaver, its menus.
export type Face = "body" | "block" | "serif"

-- TextRole names one step of the HUD type scale.
export type TextRole = "display" | "title" | "body" | "label" | "caption"

-- Type is one type role: its face, size in pixels, and whether in capitals.
export type Type = {
	-- face is the role's typeface.
	face: Face,
	-- size is its size in pixels.
	size: number,
	-- uppercase sets it in capitals.
	uppercase: boolean,
}

-- types gives every build the same typography.
theme.types = {
	-- display is the largest capitals, for a result.
	display = { face = "block", size = 46, uppercase = true } :: Type,
	-- title heads a banner or a screen.
	title = { face = "block", size = 26, uppercase = true } :: Type,
	-- body is running text.
	body = { face = "body", size = 17, uppercase = false } :: Type,
	-- label names a value or a control.
	label = { face = "block", size = 15, uppercase = true } :: Type,
	-- caption is small supporting text.
	caption = { face = "body", size = 14, uppercase = false } :: Type,
}

-- plate is the translucent plate under HUD text. It stays dark enough for
-- muted captions to read over a bright sky.
theme.plate = {
	-- color is the plate's fill.
	color = "#000000e0",
	-- radius rounds its corners.
	radius = 5,
	-- padding is [vertical, horizontal] in pixels.
	padding = { 10, 18 },
}

-- pillEdge is a pill's resting edge, a faint parchment line.
theme.pillEdge = "#ffefd71a"

-- PipShape is a round point, as in the stock round counter, or a narrow tick.
export type PipShape = "round" | "tick"

-- PipSize is one pip shape's size and corner radius, in pixels.
export type PipSize = {
	-- width and height are the pip's size.
	width: number,
	height: number,
	-- radius rounds its corners.
	radius: number,
}

-- pipShapes are each pip shape's size and corner radius, in pixels.
theme.pipShapes = {
	-- round is the stock round counter's point.
	round = { width = 18, height = 18, radius = 9 } :: PipSize,
	-- tick is a narrow mark.
	tick = { width = 7, height = 18, radius = 2 } :: PipSize,
}

-- pipRings are the stock round counter's point borders, empty and filled.
theme.pipRings = {
	-- empty is an unearned pip's ring.
	empty = "#ffefd72e",
	-- filled is an earned pip's ring.
	filled = "#10130de6",
}

-- emptyPip is an unearned pip's fill: clear, so only its faint ring shows.
theme.emptyPip = "#00000000"

-- teamBarBottom is where the stock team bar ends, in pixels on a 1080 high
-- HUD; a strip that keeps the bar sits below it.
theme.teamBarBottom = 84

-- objectiveBarBottom is where the stock objective bar under the team bar
-- ends, its urn and Rift progress, in pixels on a 1080 high HUD; a strip's
-- line goes below it so the bar's labels stay clear.
theme.objectiveBarBottom = 220

-- fpsReadoutBottom is how far down the top right corner HUD starts, in pixels
-- on a 1080 high HUD: under the game's FPS readout, whose third line, the red
-- slow server frame warning, shows during server spikes and ends about 62px
-- down.
theme.fpsReadoutBottom = 68

-- sounds are the game's interface sound events for the kit's controls: the
-- main menu's hover tick and press.
theme.sounds = {
	-- hover plays as the pointer enters a control.
	hover = "UI.MainMenu.Hover",
	-- press plays as a control is pressed.
	press = "UI.MainMenu.Activate",
}

-- motion is how long the kit's controls take to answer the pointer and a
-- meter to reach its value, in seconds: quick enough to feel immediate.
theme.motion = {
	-- hover is a control's answer to the pointer.
	hover = 0.15,
	-- meter is a meter's move to its value.
	meter = 0.25,
}

return theme
