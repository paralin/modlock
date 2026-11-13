package ui

// Flow places an element's children: Down in a column, Right in a row, and
// DownWrap or RightWrap in columns or rows that wrap where one is full; unset
// stacks them.
type Flow string

// Align places an element in its parent, or text in its element: Left, Center
// or Right across, and Top, Center or Bottom down.
type Align string

// Overflow decides what happens to children that do not fit: Squish shrinks
// them, Clip cuts them off, Scroll lets the player scroll to them, and Visible
// draws them outside the element.
type Overflow string

// Face is one of the game's typefaces: Body its text, Block the capitals of
// its clock and titles, Serif its menus.
type Face string

// Weight sets text's weight in place of Bold: Thin, Light, Normal, Medium,
// SemiBold, Bold or Black.
type Weight string

// TextOverflow ends text that does not fit with an Ellipsis, Shrinks it until
// it fits, or Clips it at the edge.
type TextOverflow string

// Decoration draws a line Underline or through (Strike) the text.
type Decoration string

// Shape is the direction a gradient's colors run: Down, Right, Up, Left or
// Radial.
type Shape string

// Easing is a transition's pace: EaseOut, EaseIn, EaseInOut or Linear.
type Easing string

// Scaling fits a picture into its element's box: Fit shows it whole, Cover
// fills the box and crops, Stretch fills the box and stretches, FitWidth and
// FitHeight fit one side, and None draws it at its own size.
type Scaling string

// HeroLook is one of the game's pictures of a hero: Small the square portrait
// of the kill feed, Card the hero picker's tall card, Icon the round icon,
// Vertical the hero grid's narrow portrait, TopBar the top bar's portrait and
// Minimap the minimap icon.
type HeroLook string

// HudPart is a part of the game's own HUD: LevelUp the notice for each boon,
// ability point and power increase, OwnHealth the player's health bar, TopBar
// the bar of team scores, portraits and clock, Ammo the clip ring and count,
// Souls the souls and level readout, Minimap the minimap, Abilities the
// ability and active item bar, Status the game's mark in the top-left corner,
// Stats the weapon, spirit and vitality stats, HeroPicker the screen a player
// without a hero picks one on, BrokerCountdown the Broker's countdown, and All
// the whole HUD but the mods' own interfaces and the game's tooltips.
type HudPart string

// The names of each option. They are untyped so one name serves every option
// that takes it, as Right does a flow, an alignment and a gradient.
const (
	// Down runs children or colors down.
	Down = "down"
	// Right runs children or colors right, or aligns right.
	Right = "right"
	// DownWrap places children in columns that wrap.
	DownWrap = "downWrap"
	// RightWrap places children in rows that wrap.
	RightWrap = "rightWrap"
	// Up runs a gradient's colors up.
	Up = "up"
	// Left runs a gradient's colors left, or aligns left.
	Left = "left"
	// Radial runs a gradient's colors out from the center.
	Radial = "radial"
	// Top aligns to the top.
	Top = "top"
	// Center aligns to the middle.
	Center = "center"
	// Bottom aligns to the bottom.
	Bottom = "bottom"
	// Squish shrinks children that do not fit.
	Squish = "squish"
	// Clip cuts off children or text that do not fit.
	Clip = "clip"
	// Scroll lets the player scroll to children that do not fit.
	Scroll = "scroll"
	// Visible draws children that do not fit outside the element.
	Visible = "show"
	// Body is the game's text face.
	Body = "body"
	// Block is the face of the game's clock and titles.
	Block = "block"
	// Serif is the face of the game's menus.
	Serif = "serif"
	// Thin is the thinnest weight.
	Thin = "thin"
	// Light is a light weight.
	Light = "light"
	// Normal is the regular weight.
	Normal = "normal"
	// Medium is a medium weight.
	Medium = "medium"
	// SemiBold is a semibold weight.
	SemiBold = "semiBold"
	// Bold is a bold weight.
	Bold = "bold"
	// Black is the heaviest weight.
	Black = "black"
	// Ellipsis ends text that does not fit with an ellipsis.
	Ellipsis = "ellipsis"
	// Shrink shrinks text until it fits.
	Shrink = "shrink"
	// Underline draws a line under the text.
	Underline = "underline"
	// Strike draws a line through the text.
	Strike = "strike"
	// EaseOut starts fast and slows to the end.
	EaseOut = "out"
	// EaseIn starts slow and speeds to the end.
	EaseIn = "in"
	// EaseInOut starts and ends slow.
	EaseInOut = "inOut"
	// Linear keeps one pace.
	Linear = "linear"
	// Fit shows a picture whole.
	Fit = "fit"
	// Cover fills the box and crops.
	Cover = "cover"
	// Stretch fills the box and stretches.
	Stretch = "stretch"
	// FitWidth fits a picture's width.
	FitWidth = "fitWidth"
	// FitHeight fits a picture's height.
	FitHeight = "fitHeight"
	// None draws a picture at its own size.
	None = "none"
	// Small is the kill feed's square portrait.
	Small = "small"
	// Card is the hero picker's tall card.
	Card = "card"
	// Icon is the round icon.
	Icon = "icon"
	// Vertical is the hero grid's narrow portrait.
	Vertical = "vertical"
	// TopBar is the top bar, or its portrait.
	TopBar = "topBar"
	// Minimap is the minimap, or its icon.
	Minimap = "minimap"
	// LevelUp is the notice for each boon and ability point.
	LevelUp = "levelUp"
	// OwnHealth is the player's health bar.
	OwnHealth = "ownHealth"
	// Ammo is the clip ring and count at the crosshair.
	Ammo = "ammo"
	// Souls is the souls and level readout.
	Souls = "souls"
	// Abilities is the ability and active item bar.
	Abilities = "abilities"
	// Status is the game's mark in the top-left corner.
	Status = "status"
	// Stats is the weapon, spirit and vitality stats.
	Stats = "stats"
	// HeroPicker is the screen a player without a hero picks one on.
	HeroPicker = "heroPicker"
	// BrokerCountdown is the countdown in the Broker's window.
	BrokerCountdown = "brokerCountdown"
	// All is the whole HUD but the mods' own interfaces and tooltips.
	All = "all"
)
