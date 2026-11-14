"""hud is the HUD kit: components that draw a mod's HUD in the stock HUD's
look, from the tokens in theme. text sets VALVEPulp capitals over a soft
shadow, plates and pills hold text over a bright sky, and pips count as the
stock round counter does. The roster lists the players down the top right
corner. Actions are the game's menu buttons, with its hover and press sounds,
and a meter fills toward its value.

    show(player, top_strip(heading="Round 2")[score(points=(1, 0))])"""

from collections.abc import Callable, Sequence
from typing import Literal, NotRequired, Required, TypedDict

from . import HEROES, Player
from .theme import (
    EMPTY_PIP,
    FPS_READOUT_BOTTOM,
    MOTION,
    OBJECTIVE_BAR_BOTTOM,
    PILL_EDGE,
    PIP_RINGS,
    PIP_SHAPES,
    PLATE,
    SHADES,
    SIZES,
    SOUNDS,
    TEAM_BAR_BOTTOM,
    TONES,
    TYPES,
    PipShape,
    TextRole,
    Tone,
)
from .ui import Child, Element, Parent, Style, avatar, button, component, image, label, panel, pct


class _Arrival(TypedDict):
    """_Arrival is an element's style with the enter and exit styles that
    ease it in and out."""

    style: Style
    enter: Style
    exit: Style


def _arriving(style: Style, start: Style) -> _Arrival:
    """_arriving eases a component in from start, slightly smaller or aside,
    and fades it out as it goes, over a quarter second; style's own
    transition wins."""
    return {
        "style": {"transition": {"seconds": 0.25, "easing": "out"}, **style},
        "enter": {"opacity": 0, **start},
        "exit": {"opacity": 0},
    }


def _text(
    children: Child, role: TextRole, tone: Tone | None, plated: bool, style: Style, centered: bool = True
) -> Parent:
    """_text is one label in a type role, over the soft shadow unless plated,
    and centered in its parent unless not centered."""
    kind = TYPES[role]
    base: Style = {
        "face": kind.face,
        "font_size": kind.size,
        "uppercase": kind.uppercase,
        "shadow": not plated,
        "color": TONES[tone or ("muted" if role == "label" else "text")],
        "text_align": "center",
    }
    if centered:
        base["horizontal_align"] = "center"
    return label(style={**base, **style})[children]


@component
def text(
    children: Child, *, role: TextRole, tone: Tone | None = None, plated: bool = False, style: Style | None = None
) -> Child:
    """text is one label in a type role and tone, over the soft shadow unless
    plated. A label is muted and the rest are text unless tone is set; plated
    marks text on a plate or pill, which takes no shadow, since on the dark
    plate a shadow only smears the glyphs."""
    return _text(children, role, tone, plated, style or {})


@component
def plate(children: Child, *, flow: Literal["down", "right"] = "down", style: Style | None = None) -> Child:
    """plate is the dark plate under HUD text, readable over a bright sky."""
    base: Style = {
        "flow": flow,
        "horizontal_align": "center",
        "background": PLATE.color,
        "border_radius": PLATE.radius,
        "padding": PLATE.padding,
    }
    return panel(style={**base, **(style or {})})[children]


def _pill(children: Child, flow: Literal["down", "right"], style: Style) -> Parent:
    """_pill is the plate rounded into a capsule with a faint parchment
    edge."""
    base: Style = {
        "flow": flow,
        "horizontal_align": "center",
        "background": PLATE.color,
        "border_radius": 50,
        "border_width": 2,
        "border_color": PILL_EDGE,
        "padding": (3, 6),
    }
    return panel(style={**base, **style})[children]


@component
def pill(children: Child, *, flow: Literal["down", "right"] = "right", style: Style | None = None) -> Child:
    """pill is the plate rounded into a capsule with a faint parchment edge,
    flowing right: the counter under a top strip's heading."""
    return _pill(children, flow, style or {})


def pips(
    *,
    filled: int,
    total: int,
    tone: Tone,
    shape: PipShape = "round",
    mirror: bool = False,
    style: Style | None = None,
) -> Element:
    """pips shows filled out of total as filled and empty pips, as the stock
    round counter: round points or narrow ticks. mirror fills from the right,
    for the far side of a counter."""
    size = PIP_SHAPES[shape]

    def lit(index: int) -> bool:
        return index >= total - filled if mirror else index < filled

    base: Style = {"flow": "right", "vertical_align": "center"}
    return panel(style={**base, **(style or {})})[
        [
            panel(
                key=f"pip-{index}",
                style={
                    "width": size.width,
                    "height": size.height,
                    "border_radius": size.radius,
                    "margin": (0, 2),
                    "background": TONES[tone] if lit(index) else EMPTY_PIP,
                    "border_width": 2,
                    "border_color": PIP_RINGS.filled if lit(index) else PIP_RINGS.empty,
                },
            )
            for index in range(total)
        ]
    ]


@component
def tag(
    children: Child, *, tone: Tone, fill: Literal["outline", "filled"] = "outline", style: Style | None = None
) -> Child:
    """tag is small heading capitals in a 1px border of its tone: outlined for
    a status read in a list, filled for a result."""
    filled = fill == "filled"
    base: Style = {
        "face": "block",
        "font_size": 12,
        "letter_spacing": 1,
        "uppercase": True,
        "color": TONES["on_accent"] if filled else TONES[tone],
        "border_width": 1,
        "border_color": TONES[tone],
        "border_radius": SIZES.radius,
        "padding": (4, 8),
        "vertical_align": "center",
    }
    if filled:
        base["background"] = TONES[tone]
    return label(style={**base, **(style or {})})[children]


def banner(*, title: str, detail: str | None = None, tone: Tone | None = None, style: Style | None = None) -> Element:
    """banner announces a moment: a centered title in the heading face with a
    muted detail under it. It grows in as it appears and fades as it goes."""
    base: Style = {"flow": "down", "horizontal_align": "center", "margin": (20, 0, 0, 0)}
    return panel(**_arriving({**base, **(style or {})}, {"scale": 0.92}))[
        _text(title, "title", tone, False, {}),
        detail and _text(detail, "caption", "muted", False, {}),
    ]


@component
def top_strip(
    children: Child,
    *,
    heading: str | None = None,
    line: str | None = None,
    line_tone: Tone = "text",
    keep_team_bar: bool = False,
) -> Child:
    """top_strip is the top block of an arena HUD: heading in spaced capitals
    names the mode or round and the lane, over the counter pill that holds
    the children, such as score and pips, and line reads under both, such as
    a clock or the news. keep_team_bar seats the strip under the stock team
    bar, which keeps its clock, heroes and player slots; there the heading and
    pill fit above the stock objective bar and the line goes below that bar.
    Unset, the strip takes the bar's place."""
    # The heading sits over the counter pill, and the line reads under both.
    block = [
        heading and _text(heading, "label", None, False, {"letter_spacing": 1}),
        _pill(children, "right", {"margin": (4, 0, 0, 0)}),
    ]
    spaced: Style = {"margin": (4, 0, 0, 0), "font_size": 18, "letter_spacing": 1}
    under = line and _text(line, "label", line_tone, False, spaced)

    # A column centers its parts from top down. In the bar's place, one column
    # holds them all; under the kept bar, the line has its own below the
    # objective bar.
    def column(top: int, children: Child) -> Parent:
        return panel(style={"flow": "down", "horizontal_align": "center", "margin": (top, 0, 0, 0)})[children]

    if not keep_team_bar:
        return column(14, [*block, under])
    return panel(style={"width": "fill", "height": "fill"})[
        column(TEAM_BAR_BOTTOM, block),
        under and column(OBJECTIVE_BAR_BOTTOM, under),
    ]


def score(*, points: tuple[int, int], tones: tuple[Tone, Tone] = ("accent", "text")) -> Element:
    """score is a counter's points, the near side first, each side in its
    tone, in the title face; the near side is the accent unless tones is set.
    It sits in a pill, so its text is plated."""

    # Each part drops the text's centering, so the row lays them out.
    def side(value: Child, tone: Tone, margin: bool) -> Parent:
        return _text(value, "title", tone, True, {"margin": (0, 10)} if margin else {}, centered=False)

    return panel(style={"flow": "right", "vertical_align": "center"})[
        side(points[0], tones[0], True),
        side(":", "muted", False),
        side(points[1], tones[1], True),
    ]


class RosterRow(TypedDict):
    """RosterRow is one player as the roster shows them. hero is the
    player's hero as Pawn.hero gives it, or 0 without one; activity is the
    line under the name, such as "Free roam". self marks the viewing player's
    own row, and steam_id badges the portrait's corner with the player's Steam
    avatar. on_press makes the row a button the viewer presses while they have
    a cursor, such as to open a card about that player, and selected edges the
    portrait in the accent, as for the row a card shows."""

    name: Required[str]
    hero: Required[int]
    activity: Required[str]
    self: NotRequired[bool]
    steam_id: NotRequired[int]
    on_press: NotRequired[Callable[[Player], object]]
    selected: NotRequired[bool]


# ROSTER_ROWS is the most players the roster shows, above the minimap.
ROSTER_ROWS = 6

# _ROSTER_NAME is the most characters a roster name shows.
_ROSTER_NAME = 24

# _PORTRAIT_SIZE is the portrait square's side, its 1px edge included.
_PORTRAIT_SIZE = 44

# _BADGE_SIZE is the side of the avatar badge on a portrait's corner.
_BADGE_SIZE = 18

# _PORTRAITS maps each hero's id to its small portrait.
_PORTRAITS = {each.id: each.portrait for each in HEROES}


def roster_key(rows: Sequence[RosterRow]) -> str:
    """roster_key is what the roster shows, to redraw only when it
    changes."""
    return "\n".join(
        f"{'*' if row.get('self') else ''}{'+' if row.get('selected') else ''}"
        f"{'>' if row.get('on_press') else ''}{row['name']}\t{row['hero']}\t{row['activity']}"
        f"\t{row.get('steam_id', '')}"
        for row in rows[:ROSTER_ROWS]
    )


def roster(*, rows: Sequence[RosterRow], top: int = FPS_READOUT_BOTTOM) -> Element | None:
    """roster stacks up to ROSTER_ROWS players down the top right corner, each
    one's hero portrait at the edge with their name and a line, such as what
    they are doing or their score, to its left; top is its top margin, under
    the FPS readout unless set. It shows nothing without rows. A mode that
    hides the stock scoreboard's heroes shows who else is here with it."""
    if not rows:
        return None
    return panel(
        style={
            "width": "fit",
            "height": "fit",
            "flow": "down",
            "horizontal_align": "right",
            "vertical_align": "top",
            "margin": (top, 24, 0, 0),
        }
    )[[_roster_row(row, index) for index, row in enumerate(rows[:ROSTER_ROWS])]]


def _roster_row(row: RosterRow, index: int) -> Element:
    """_roster_row is one player: name and activity, then the portrait, whose
    edge reads in the accent on the viewing player's own row."""
    # The name and its line align right, against the portrait.
    text = panel(style={"flow": "down", "vertical_align": "center", "margin": (0, 12, 0, 0)})[
        _text(_clip(row["name"].upper(), _ROSTER_NAME), "label", "text", False, _RIGHT),
        _text(row["activity"], "caption", "muted", False, {**_RIGHT, "margin": (1, 0, 0, 0)}),
    ]

    # The portrait sits in a 1px edge on a raised square, empty without a
    # hero, with the player's avatar on its lower left corner.
    src = _PORTRAITS.get(row["hero"])
    steam_id = row.get("steam_id")
    portrait = panel(
        style={
            "width": _PORTRAIT_SIZE,
            "height": _PORTRAIT_SIZE,
            "padding": 1,
            "vertical_align": "center",
            "background": TONES["accent"] if row.get("self") or row.get("selected") else SHADES.dim,
        }
    )[
        panel(style={"width": _FULL, "height": _FULL, "background": SHADES.raised})[
            src is not None and image(src=src, style={"width": _FULL, "height": _FULL}),
            steam_id is not None and _avatar_badge(steam_id),
        ]
    ]

    # A player who joins slides in from the edge. A pressable row is a button
    # whose ground lifts under the pointer.
    style: Style = {"flow": "right", "horizontal_align": "right", "margin": (0, 0, 8, 0)}
    on_press = row.get("on_press")
    if on_press is None:
        return panel(key=f"row-{index}", **_arriving(style, {"translate": (16, 0)}))[text, portrait]
    pressable = _arriving(
        {**style, "border_radius": SIZES.radius, "transition": MOTION.hover}, {"translate": (16, 0)}
    )
    return button(
        key=f"row-{index}",
        hover={"background": SHADES.raised},
        hover_sound=SOUNDS.hover,
        sound=SOUNDS.press,
        on_press=on_press,
        **pressable,
    )[text, portrait]


# _RIGHT aligns a roster row's text right, against the portrait.
_RIGHT: Style = {"horizontal_align": "right", "text_align": "right"}

# _FULL is the whole of the parent.
_FULL = pct(100)


def _avatar_badge(steam_id: int) -> Element:
    """_avatar_badge is a player's avatar on the lower left corner of a
    portrait."""
    return avatar(
        steam_id=steam_id,
        style={
            "width": _BADGE_SIZE,
            "height": _BADGE_SIZE,
            "horizontal_align": "left",
            "vertical_align": "bottom",
            "border_radius": _BADGE_SIZE / 2,
            "border_width": 1,
            "border_color": SHADES.dim,
        },
    )


def _clip(text: str, width: int) -> str:
    """_clip shortens text to at most width characters, at a word break where
    one falls in the cut, and marks the cut with an ellipsis."""
    if len(text) <= width:
        return text
    cut = text[: width - 1]
    space = cut.rfind(" ")
    return f"{(cut[:space] if space > 0 else cut).rstrip()}…"


class _ActionLook(TypedDict):
    """_ActionLook is one kind of action's fill, edge and text, idle and
    hovered."""

    idle: Style
    hover: Style
    text: str


# _ACTION_LOOKS are each kind of action's look.
_ACTION_LOOKS: dict[str, _ActionLook] = {
    "primary": {"idle": {"background": TONES["text"]}, "hover": {"background": SHADES.white}, "text": SHADES.ink},
    "secondary": {
        "idle": {"background": SHADES.raised, "border_width": 1, "border_color": SHADES.faint},
        "hover": {"background": SHADES.lit, "border_color": TONES["text"]},
        "text": TONES["text"],
    },
    "quiet": {"idle": {"background": SHADES.clear}, "hover": {"background": SHADES.raised}, "text": SHADES.quiet},
}


@component
def action(
    children: Child,
    *,
    kind: Literal["primary", "secondary", "quiet"] = "secondary",
    disabled: bool = False,
    on_press: Callable[[Player], object] | None = None,
    style: Style | None = None,
) -> Child:
    """action is a button in the game's menu look: capitals on a fill that
    lights as the pointer enters, with the main menu's hover and press sounds.
    kind is "primary" for the one action a screen leads with, "secondary" for
    the rest, and "quiet" for an action that should not draw the eye. A
    disabled action dims and takes no presses."""
    look = _ACTION_LOOKS[kind]
    text = label(
        style={
            "face": "block",
            "font_size": 16,
            "uppercase": True,
            "letter_spacing": 1,
            "color": look["text"],
            "horizontal_align": "center",
            "vertical_align": "center",
            "no_wrap": True,
        }
    )[children]
    base: Style = {
        "height": SIZES.control,
        "padding": (0, 14),
        "border_radius": SIZES.radius,
        "transition": MOTION.hover,
        **look["idle"],
    }
    if disabled:
        base["opacity"] = 0.4
        return button(style={**base, **(style or {})}, sound=SOUNDS.press, disabled=True)[text]
    return button(
        style={**base, **(style or {})},
        hover=look["hover"],
        hover_sound=SOUNDS.hover,
        sound=SOUNDS.press,
        on_press=on_press,
    )[text]


def meter(
    *, value: float, tone: Tone = "accent", width: float = 160, height: float = 8, style: Style | None = None
) -> Element:
    """meter is a bar that fills from the left toward value, from 0 to 1,
    easing to each new value: a charge, a capture or a timer. It is 160 by 8
    pixels unless width and height are set."""
    full = min(1.0, max(0.0, value))
    base: Style = {
        "width": width,
        "height": height,
        "background": SHADES.raised,
        "border_radius": height / 2,
        "overflow": "clip",
    }
    return panel(style={**base, **(style or {})})[
        # The fill scales rather than resizes, so it eases without layout work.
        panel(
            style={
                "width": _FULL,
                "height": _FULL,
                "background": TONES[tone],
                "scale": (full, 1),
                "origin": (0, 50),
                "transition": MOTION.meter,
            }
        )
    ]
