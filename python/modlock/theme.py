"""theme holds the HUD kit's tokens, each named once: the colors, type scale
and sizes the kit's components draw with, and that a mod's own panels can
share. They follow the stock HUD's look: VALVEPulp capitals and Retail Demo
text in off-white with a dark shadow, team colors and translucent black
plates."""

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Literal

# Tone names the colors a surface may use for meaning.
type Tone = Literal[
    "text",
    "muted",
    "accent",
    "on_accent",
    "amber",
    "sapphire",
    "gas",
    "success",
    "danger",
    "gold",
]

# TONES maps each meaning to one color. Text, team and danger colors are the
# stock HUD's offWhite, team1Color, team2Color and colorEnemy.
TONES: Mapping[Tone, str] = {
    "text": "#ffefd7",
    "muted": "#bfb3a1",
    # The accent marks the player's own choices.
    "accent": "#fd736d",
    # Text on an accent fill.
    "on_accent": "#070709",
    "amber": "#d4860b",
    "sapphire": "#4d75c3",
    # The gas of a closing arena.
    "gas": "#3ee08f",
    # Done, under par, ready and live: a step darker than gas, so status text
    # and tags sit calmer on the plates.
    "success": "#36c47f",
    "danger": "#ff410d",
    "gold": "#ebc934",
}

# TEAM_TONES colors each game team, by its team number: 2 is the Hidden King's
# amber, 3 the Archmother's sapphire.
TEAM_TONES: Mapping[int, Tone] = {2: "amber", 3: "sapphire"}


@dataclass(frozen=True, slots=True)
class Shades:
    """Shades are the neutrals a full screen draws with, from the stock main
    menu's base styles, so a full screen reads as part of the game's own
    menus. Fills are opaque offWhite tints of the offBlack backdrop: Panorama
    blends translucent fills far stronger than their alpha, which let the
    stock menu show through."""

    # quiet is offWhite at about half strength: idle navigation and secondary
    # text.
    quiet: str = "#ffefd78c"
    # faint is offWhite at the popup button's 44%: idle pill borders.
    faint: str = "#ffefd770"
    # dim is offWhite at about 40%: counts, placeholders and offline players.
    dim: str = "#6b675d"
    # hairline is offWhite at 13%: hairlines between sections and plate edges.
    hairline: str = "#2b2a24"
    # ink is offBlack: text on light fills, and the backdrop's hue.
    ink: str = "#10130d"
    # backdrop is the menu backdrop: an opaque offBlack that hides the stock
    # menu.
    backdrop: str = "#0b0d09"
    # veil is the backdrop at 95%, under a panel the player reads over the
    # game.
    veil: str = "#0b0d09f2"
    # plate is offWhite at 4%: a plate on the backdrop.
    plate: str = "#151611"
    # raised is offWhite at 8%: a row or stat inside a plate.
    raised: str = "#1e1f19"
    # lit is offWhite at 13%: a hovered or joined row.
    lit: str = "#2b2a24"
    # white is the full-strength fill of a lit primary button.
    white: str = "#ffffff"
    # clear is no fill or border, where a state turns one off.
    clear: str = "#00000000"


# SHADES are the kit's neutrals.
SHADES = Shades()


@dataclass(frozen=True, slots=True)
class Sizes:
    """Sizes holds the shared sizes in pixels: panel buttons and fields share
    one height, row buttons a shorter one, and every control and tag one
    corner radius."""

    # control is the height of panel buttons and fields.
    control: int = 32
    # row_control is the height of a button inside a list row.
    row_control: int = 28
    # row is the height of one list row.
    row: int = 32
    # radius rounds buttons, fields and tags.
    radius: int = 4
    # panel_radius rounds panels.
    panel_radius: int = 8


# SIZES are the kit's shared sizes.
SIZES = Sizes()

# Face is one of the game's typefaces: body is Retail Demo, its text; block is
# VALVEPulp, set in capitals for the stock clock, round titles and callouts;
# serif is Reaver, its menus.
type Face = Literal["body", "block", "serif"]

# TextRole names one step of the HUD type scale.
type TextRole = Literal["display", "title", "body", "label", "caption"]


@dataclass(frozen=True, slots=True)
class Type:
    """Type is one type role: its face, size in pixels, and whether in
    capitals."""

    face: Face
    size: int
    uppercase: bool


# TYPES gives every build the same typography.
TYPES: Mapping[TextRole, Type] = {
    "display": Type("block", 46, True),
    "title": Type("block", 26, True),
    "body": Type("body", 17, False),
    "label": Type("block", 15, True),
    "caption": Type("body", 14, False),
}


@dataclass(frozen=True, slots=True)
class Plate:
    """Plate is the translucent plate under HUD text. It stays dark enough for
    muted captions to read over a bright sky."""

    color: str = "#000000e0"
    radius: int = 5
    # padding is (vertical, horizontal) in pixels.
    padding: tuple[int, int] = (10, 18)


# PLATE is the plate under HUD text.
PLATE = Plate()

# PILL_EDGE is a pill's resting edge, a faint parchment line.
PILL_EDGE = "#ffefd71a"

# PipShape is a round point, as in the stock round counter, or a narrow tick.
type PipShape = Literal["round", "tick"]


@dataclass(frozen=True, slots=True)
class PipSize:
    """PipSize is one pip shape's size and corner radius, in pixels."""

    width: int
    height: int
    radius: int


# PIP_SHAPES are each pip shape's size.
PIP_SHAPES: Mapping[PipShape, PipSize] = {
    "round": PipSize(18, 18, 9),
    "tick": PipSize(7, 18, 2),
}


@dataclass(frozen=True, slots=True)
class PipRings:
    """PipRings are the stock round counter's point borders, empty and
    filled."""

    empty: str = "#ffefd72e"
    filled: str = "#10130de6"


# PIP_RINGS are the round counter's borders.
PIP_RINGS = PipRings()

# EMPTY_PIP is an unearned pip's fill: clear, so only its faint ring shows.
EMPTY_PIP = "#00000000"

# TEAM_BAR_BOTTOM is where the stock team bar ends, in pixels on a 1080 high
# HUD; a strip that keeps the bar sits below it.
TEAM_BAR_BOTTOM = 84

# OBJECTIVE_BAR_BOTTOM is where the stock objective bar under the team bar
# ends, its urn and Rift progress, in pixels on a 1080 high HUD; a strip's line
# goes below it so the bar's labels stay clear.
OBJECTIVE_BAR_BOTTOM = 220

# FPS_READOUT_BOTTOM is how far down the top right corner HUD starts, in
# pixels on a 1080 high HUD: under the game's FPS readout, whose third line,
# the red slow server frame warning, shows during server spikes and ends about
# 62px down.
FPS_READOUT_BOTTOM = 68


@dataclass(frozen=True, slots=True)
class Sounds:
    """Sounds are the game's interface sound events for the kit's controls:
    the main menu's hover tick and press."""

    hover: str = "UI.MainMenu.Hover"
    press: str = "UI.MainMenu.Activate"


# SOUNDS are the kit's control sounds.
SOUNDS = Sounds()


@dataclass(frozen=True, slots=True)
class Motion:
    """Motion is how long the kit's controls take to answer the pointer and a
    meter to reach its value, in seconds: quick enough to feel immediate."""

    hover: float = 0.15
    meter: float = 0.25


# MOTION is the kit's timing.
MOTION = Motion()
