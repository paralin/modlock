"""ui shows each player an interface built from elements. Each element is a
call that takes its props by keyword, and an element that holds others takes
them in square brackets:

    from modlock.ui import button, label, panel, show

    show(player, panel(style={"flow": "down", "horizontal_align": "center", "margin": (80, 0, 0, 0)})[
        label(style={"font_size": 32, "bold": True})[f"Round {round}"],
        button(on_press=ready)["Ready"],
    ])

The player's game draws each element with a stock panel, so no mod code runs
there. show sends only the elements that changed since the last call for that
player, so a mod may call it whenever its state changes, or every frame.

Every element takes a style and a hover style, which the game draws while the
pointer is over it and animates to when the style has a transition. A tooltip
and a hover sound show and play on the same hover; a button plays its sound
when pressed. A hero element shows one of the game's pictures of a hero, such
as hero(hero=pawn.hero, look="small").

An element's enter style is where its panel starts when it first shows, and
its exit style where it goes as it leaves; its transition eases each way, so
panel(style={"opacity": 1, "transition": 0.2}, enter={"opacity": 0}) fades in.
A cycle swings an element to another style and back while it shows, as a
pulse or a blink.

Inputs take a value from the player and call on_change with the player and the
value the host checked: entry a line of text, slider a number in its range,
toggle on or off, and dropdown the index of an option. The value the mod draws
is the truth; the player's edit shows until the mod's next value for that
input differs from its last.

A label with a token shows the game's own text in the player's language, such
as label(token="#Citadel_HeroName_Haze"), and an avatar shows a player's Steam
picture: avatar(steam_id=connection.steam_id).

A layout element loads a layout from the mod's content addon, such as
layout(src="shop.xml", data=stock). Its script runs in the player's game, so
only a host that has reviewed the mod's content loads it; elsewhere it draws
nothing. The script includes the Panorama library scripts/modlock/layout.js
and reads data with Modlock.data.

A component is a plain function that returns elements. One decorated with
component takes its children in square brackets as an element does.

route sends a player a named JSON value that every layout of the mod reads
with Modlock.route, wherever it sits in the tree. toast shows a player a short
notice at the lower left, which fades after a few seconds. hide_hud hides parts
of the game's own HUD from a player until the mod lists them no more, hides
the player's interface, or stops; Modlock gives them back on its own when the
player leaves. ui_sound plays one of the game's sound events in one player's
game alone. sheet adds a style sheet from the mod's content addon to every
interface the mod shows, and an element's class_ names the sheet's classes."""

import json
import re
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any, Concatenate, Literal, NewType, NotRequired, Required, TypedDict, Unpack

from . import (
    _SCHEMA,  # pyright: ignore[reportPrivateUsage]
    HEROES,
    Player,
    Wheel,
    _call,  # pyright: ignore[reportPrivateUsage]
    _serve_ui,  # pyright: ignore[reportPrivateUsage]
    _UiHandlers,  # pyright: ignore[reportPrivateUsage]
    players,
    read_field,
    wire,
)

# Percent is a length in percent of the parent, as pct makes it.
Percent = NewType("Percent", str)


def pct(value: float) -> Percent:
    """pct returns value percent of the parent as a length, such as pct(50)."""
    return Percent(f"{_js_text(value)}%")


# Length is a size: pixels, a percent of the parent from pct, "fit" to size
# to the children, or "fill" to share the space left in the parent's flow.
type Length = float | Percent | Literal["fit", "fill"]

# Edges are distances in pixels: one for every side, (vertical, horizontal),
# or (top, right, bottom, left).
type Edges = float | tuple[float, float] | tuple[float, float, float, float]

# Overflow decides what happens to children that do not fit: "squish" shrinks
# them, "clip" cuts them off, "scroll" lets the player scroll to them, and
# "show" draws them outside the element.
type Overflow = Literal["squish", "clip", "scroll", "show"]


class Gradient(TypedDict, total=False):
    """Gradient blends colors across an element in place of its background.
    The colors run down by default; at places each color from 0 to 1, spaced
    evenly when unset."""

    shape: Literal["down", "right", "up", "left", "radial"]
    colors: Required[Sequence[str]]
    at: Sequence[float]


class Shadow(TypedDict, total=False):
    """Shadow is a soft shadow, offset by x and y and softened by blur, in
    pixels. Spread grows a box's shadow in pixels, or strengthens a text
    shadow, where 0 draws it at full strength; inset draws a box's shadow
    inside its edge."""

    x: float
    y: float
    blur: float
    spread: float
    color: Required[str]
    inset: bool


class Transition(TypedDict, total=False):
    """Transition animates each change of an element's style, and the move to
    its hover style and back, over seconds after delay. Easing is "out" unless
    set."""

    seconds: Required[float]
    delay: float
    easing: Literal["out", "in", "in_out", "linear"]


class Style(TypedDict, total=False):
    """Style lays out and paints an element. Colors are "#rrggbb" or
    "#rrggbbaa".

    min_width, max_width, min_height and max_height bound the element's size.
    flow places the children in a column or a row, or in columns or rows that
    wrap where one is full; unset stacks them. x and y place the element at an
    offset from its parent's top left corner, in pixels or a percent of the
    parent, outside the parent's flow. z_index draws the element above its
    siblings with a lower index. overflow decides what children that do not
    fit do, across and down; overflow_x and overflow_y set it for one
    direction. opacity fades the element and its children, from 0 to 1.

    face is one of the game's typefaces: "body" its text, "block" the capitals
    of its clock and titles, "serif" its menus. uppercase sets the text in
    capitals, letter_spacing widens the space between letters in pixels, and
    shadow draws a soft drop shadow under the text, to read over the world;
    text_shadow draws this shadow in place of shadow's. border_width and
    border_color draw a line around the element's edge; it shows only when
    both are set. gradient fills the element in place of its background, and
    box_shadow draws a shadow around or inside its box. tint washes the
    element and its children with a color, recoloring a white icon;
    saturation, brightness and contrast scale the colors, where 1 leaves
    them; blur blurs by a radius in pixels. weight sets the text's weight in
    place of bold, line_height is the height of each line in pixels, no_wrap
    keeps the text on one line, and text_overflow ends text that does not fit
    with an "ellipsis", "shrink"s it until it fits, or "clip"s it.
    decoration draws a line under or through the text.

    translate moves the element as drawn by (x, y) pixels, scale scales it by
    one factor or (x, y), rotate turns it clockwise in degrees, and origin is
    the point it scales and turns about, as (x, y) percents of its size;
    unset is its center. transition animates each change of the style:
    seconds, or a Transition. Animate transform, opacity and colors; size and
    placement animate too, but move the element's neighbors each frame."""

    width: Length
    height: Length
    min_width: Length
    max_width: Length
    min_height: Length
    max_height: Length
    flow: Literal["down", "right", "down_wrap", "right_wrap"]
    x: float | Percent
    y: float | Percent
    z_index: int
    overflow: Overflow
    overflow_x: Overflow
    overflow_y: Overflow
    horizontal_align: Literal["left", "center", "right"]
    vertical_align: Literal["top", "center", "bottom"]
    margin: Edges
    padding: Edges
    background: str
    color: str
    font_size: float
    bold: bool
    text_align: Literal["left", "center", "right"]
    border_radius: float
    opacity: float
    face: Literal["body", "block", "serif"]
    uppercase: bool
    letter_spacing: float
    shadow: bool
    text_shadow: Shadow
    border_width: float
    border_color: str
    gradient: Gradient
    box_shadow: Shadow
    tint: str
    saturation: float
    brightness: float
    contrast: float
    blur: float
    italic: bool
    weight: Literal["thin", "light", "normal", "medium", "semi_bold", "bold", "black"]
    line_height: float
    no_wrap: bool
    text_overflow: Literal["ellipsis", "shrink", "clip"]
    decoration: Literal["underline", "strike"]
    translate: tuple[float, float]
    scale: float | tuple[float, float]
    rotate: float
    origin: tuple[float, float]
    transition: float | Transition


# HudPart is a part of the game's own HUD: "level_up" the notice for each
# boon, ability point and power increase, "own_health" the player's health bar
# with its shields and regeneration, "top_bar" the bar of team scores,
# portraits and clock across the top, "ammo" the clip ring, count and ammo
# warnings at the crosshair, "souls" the souls and level readout, "minimap" the
# minimap with its hints and district name, "abilities" the ability and active
# item bar, "status" the game's mark in the top-left corner, "stats" the
# weapon, spirit and vitality stats at the lower left, "hero_picker" the screen
# a player without a hero picks one on, "broker_countdown" the countdown in the
# Broker's window, and "all" the whole HUD but the mods' own interfaces and the
# game's tooltips, for a screen that takes the player's whole view. Whatever a
# part does not name stays, such as the crosshair and chat.
type HudPart = Literal[
    "level_up",
    "own_health",
    "top_bar",
    "ammo",
    "souls",
    "minimap",
    "abilities",
    "status",
    "stats",
    "hero_picker",
    "broker_countdown",
    "all",
]

# Scaling fits a picture into its element's box: "fit" shows it whole, "cover"
# fills the box and crops, "stretch" fills the box and stretches, "fit_width"
# and "fit_height" fit one side, and "none" draws it at its own size.
type Scaling = Literal["fit", "cover", "stretch", "fit_width", "fit_height", "none"]

# HeroLook is one of the game's pictures of a hero: "small" the square
# portrait of the kill feed, "card" the hero picker's tall card, "icon" the
# round icon, "vertical" the hero grid's narrow portrait, "top_bar" the top
# bar's portrait and "minimap" the minimap icon.
type HeroLook = Literal["small", "card", "icon", "vertical", "top_bar", "minimap"]


class AbilityTip(TypedDict):
    """AbilityTip is the game's own window for an ability, in the player's
    language: name is the ability's class name, such as "ability_bebop_hook",
    and player is whose hero the window reads its numbers from, a hero that
    stands on the map, such as a bot that plays it."""

    name: str
    player: Player


class Cycle(TypedDict):
    """Cycle swings an element from its style to style and back, each way
    taking seconds, count times or, without a count, while it shows."""

    style: Style
    seconds: float
    count: NotRequired[int]


class ElementProps(TypedDict, total=False):
    """ElementProps are the props every drawn element takes. A handler prop,
    such as on_press, may be None for none. key names the
    element among its siblings in a list, so it keeps its panel as the list
    changes. class_ names the element's classes in the mod's sheets, separated
    by spaces. hover is the style drawn over style while the pointer is over
    the element; tooltip is the text the game's tooltip shows then, item names
    an item by class name, such as "upgrade_ability_power_shard", whose shop
    card shows instead, ability names an ability whose window shows instead of
    either, and hover_sound is the game's sound event played as the pointer
    enters, such as "UI.MainMenu.Hover". An element with any of them takes the
    pointer, so the game behind it does not. enter is the style the element's
    panel starts from when it first shows, exit the style it eases to as it
    leaves, and cycle swings it to another style and back while it shows."""

    key: str | int
    class_: str
    style: Style
    hover: Style
    tooltip: str
    item: str
    ability: AbilityTip
    hover_sound: str
    enter: Style
    exit: Style
    cycle: Cycle


class LabelProps(ElementProps, total=False):
    """LabelProps show text: the label's text children, joined, or the game's
    text for token in the player's language, with each {s:name} place in it
    filled from variables."""

    token: str
    variables: Mapping[str, str | float]


class AvatarProps(ElementProps, total=False):
    """AvatarProps show the Steam picture of the player with steam_id."""

    steam_id: Required[int]


class InputProps(ElementProps, total=False):
    """InputProps are the props every input takes. A disabled input takes no
    changes."""

    disabled: bool


class EntryProps(InputProps, total=False):
    """EntryProps make a box the player types a line into: value is its text,
    placeholder the faint text it shows empty, and max_length its limit in
    characters, 100 unless set, and at most 240 bytes of text."""

    value: str
    placeholder: str
    max_length: int
    on_change: Callable[[Player, str], object] | None


class SliderProps(InputProps, total=False):
    """SliderProps make a bar the player drags to a number from min to max, 0
    to 1 unless set, stopping at each step when step is set. on_wheel receives
    one wheel step and its pointer anchor while enabled."""

    value: float
    min: float
    max: float
    step: float
    on_change: Callable[[Player, float], object] | None
    on_wheel: Callable[[Player, Wheel], object] | None


class ToggleProps(InputProps, total=False):
    """ToggleProps make a box the player checks, labeled by its text
    children."""

    on: bool
    on_change: Callable[[Player, bool], object] | None


class DropdownProps(InputProps, total=False):
    """DropdownProps let the player pick one of options; selected is its
    index."""

    options: Required[Sequence[str]]
    selected: int
    on_change: Callable[[Player, int], object] | None


class ImageProps(ElementProps, total=False):
    """ImageProps show an image from the game's content, such as
    "file://{images}/hud/icon.png"."""

    src: Required[str]
    scaling: Scaling


class HeroProps(ElementProps, total=False):
    """HeroProps show a picture of hero, by the game's hero id."""

    hero: Required[int]
    look: HeroLook
    scaling: Scaling


class ButtonProps(ElementProps, total=False):
    """ButtonProps make a button, which holds its children and calls on_press
    with the player who pressed it. Players press buttons while they have a
    cursor. sound is the game's sound event the press plays, such as
    "UI.MainMenu.Activate", and a disabled button takes no presses."""

    sound: str
    disabled: bool
    on_press: Callable[[Player], object] | None


class LayoutProps(TypedDict, total=False):
    """LayoutProps load a layout from the mod's content addon: src is its path
    there, such as "shop.xml", and data is the JSON value its script reads
    with Modlock.data."""

    key: str | int
    class_: str
    style: Style
    src: Required[str]
    data: object


class Element:
    """Element is one element of an interface. A component's element is a
    fragment, which draws what the component returned."""

    __slots__ = ("_kind", "_props", "_key", "_children", "_render")

    def __init__(
        self,
        kind: str,
        props: Mapping[str, Any],
        children: "Child" = None,
        render: Callable[["Child"], "Child"] | None = None,
    ) -> None:
        key = props.get("key")
        self._kind = kind
        self._props = props
        self._key = None if key is None else _js_text(key)
        self._children: Child = _freeze(children)
        self._render = render

    def __repr__(self) -> str:
        return f"<{self._kind} {dict(self._props)!r} {self._children!r}>"


class Parent(Element):
    """Parent is an element that holds others, given in square brackets:
    panel()[label()["Hi"], "there"]."""

    __slots__ = ()

    def __getitem__(self, children: "Child") -> "Parent":
        return Parent(self._kind, self._props, children, self._render)


# Child is anything an element may hold. Text and numbers become labels;
# None and booleans draw nothing, so a condition can stand in a list.
type Child = Element | str | int | float | bool | None | Iterable[Child]


def _freeze(child: Child) -> Child:
    """_freeze turns every iterable in child into a tuple, so a generator lays
    out again on each draw."""
    if child is None or isinstance(child, (Element, str, int, float, bool)):
        return child
    return tuple(_freeze(item) for item in child)


def panel(**props: Unpack[ElementProps]) -> Parent:
    """panel groups its children."""
    return Parent("panel", props)


def label(**props: Unpack[LabelProps]) -> Parent:
    """label shows its text children, joined, or the game's text for its
    token."""
    return Parent("label", props)


def image(**props: Unpack[ImageProps]) -> Element:
    """image shows a picture from the game's content."""
    return Element("image", props)


def hero(**props: Unpack[HeroProps]) -> Element:
    """hero shows one of the game's pictures of a hero."""
    return Element("hero", props)


def avatar(**props: Unpack[AvatarProps]) -> Element:
    """avatar shows a player's Steam picture."""
    return Element("avatar", props)


def button(**props: Unpack[ButtonProps]) -> Parent:
    """button holds its children and calls on_press when pressed."""
    return Parent("button", props)


def entry(**props: Unpack[EntryProps]) -> Element:
    """entry is a box the player types a line into."""
    return Element("entry", props)


def slider(**props: Unpack[SliderProps]) -> Element:
    """slider is a bar the player drags to a number."""
    return Element("slider", props)


def toggle(**props: Unpack[ToggleProps]) -> Parent:
    """toggle is a box the player checks, labeled by its text children."""
    return Parent("toggle", props)


def dropdown(**props: Unpack[DropdownProps]) -> Element:
    """dropdown lets the player pick one of its options."""
    return Element("dropdown", props)


def layout(**props: Unpack[LayoutProps]) -> Element:
    """layout loads a layout from the mod's content addon."""
    return Element("layout", props)


def fragment(key: str | int | None = None) -> Parent:
    """fragment groups its children without a panel of its own; key names it
    in a list."""
    return Parent(_FRAGMENT, {} if key is None else {"key": key})


def component[**P](render: Callable[Concatenate[Child, P], Child]) -> Callable[P, Parent]:
    """component makes render, which takes the children first and its own
    props after, an element that takes its children in square brackets:

        @ui.component
        def card(children: ui.Child, *, title: str) -> ui.Child:
            return ui.panel()[ui.label()[title], children]

        card(title="Shop")[ui.button(on_press=buy)["Buy"]]"""

    def build(*args: P.args, **kwargs: P.kwargs) -> Parent:
        return Parent(_FRAGMENT, {}, None, lambda children: render(children, *args, **kwargs))

    return build


# _FRAGMENT is the kind of a component's element.
_FRAGMENT = "fragment"


def show(player: Player, root: Child) -> bool:
    """show replaces the interface the mod shows player with root. A player
    who left and a mod that restarted show nothing until the next call."""
    _view(player).root = _freeze(root)
    return _draw(player)


def hide_hud(player: Player, parts: Sequence[HudPart]) -> bool:
    """hide_hud hides parts of the game's own HUD from player until the next
    call; an empty list shows them all again."""
    _view(player).hud = list(parts)
    return _draw(player)


def free_cursor(player: Player, free: bool) -> bool:
    """free_cursor shows player's mouse cursor without Tab held while free is
    true, so they can click the mod's panels, such as a pick screen; false
    gives the mouse back to aiming. Each call with true shows the cursor
    again, even when the player put it away with the game's own keys; a
    screen that changes without a call leaves their choice be. The game does
    not stop the player's shots, camera or movement while the cursor is up: a
    screen that must not fire a weapon calls block_input for the buttons it
    takes."""
    view = _view(player)
    view.cursor = free
    if free:
        view.asks += 1
    return _draw(player)


# AbilityBarHandler takes a click on the ability in slot, 0 to 3.
type AbilityBarHandler = Callable[[Player, int], object]


def ability_bar_presses(player: Player, on_press: AbilityBarHandler | None = None) -> bool:
    """ability_bar_presses calls on_press with the slot, 0 to 3, each time
    player clicks the icon or upgrade pips of an ability on the game's own
    ability bar, as the game does when it trains one with Tab held; without
    on_press the clicks stop reaching the mod. The game answers a click from
    the hero's own abilities, and sends nothing for an ability replace_ability
    gave, so a mod that replaces abilities answers here, with train_ability. A
    click the game answers itself, with upgrade_ability, reaches the mod both
    ways. Clicks reach the mod only while its screen holds the player's HUD: a
    player who left, or whose session ended, sends none."""
    _view(player).bar = on_press
    return _draw(player)


class ShownAbility(TypedDict):
    """ShownAbility is the ability ability_display shows in one slot: name is
    its class name, whose window the game shows over it, image its picture, a
    path in the game's content as an image's src, and upgrades its bought
    tiers as Ability.upgrades, where bit 0 unlocks it."""

    name: str
    image: str
    upgrades: NotRequired[int]


class AbilityRow(TypedDict):
    """AbilityRow is the four abilities, in slot order, shown for one player;
    a None slot shows the game's own."""

    player: Player
    abilities: Sequence[ShownAbility | None]


def ability_display(viewer: Player, rows: Sequence[AbilityRow] = ()) -> bool:
    """ability_display shows viewer each row's abilities in place of the ones
    the game shows for that player's hero: on the player's row of the game's
    Tab screen, and for viewer's own row on viewer's ability bar too, in the
    game's frame, ring and pips, with the game's ability window over each. The
    game draws these from the hero's standard abilities, so one
    replace_ability gave shows as the ability it replaced. Without rows the
    game's own come back, as they do when viewer leaves or the session ends.
    Rows match the Tab screen's by the player's name and else by the hero's,
    which a bot's row shows, so each row's player should have a hero."""
    _view(viewer).display = list(rows[:64])
    return _draw(viewer)


def _display_rows(viewer: Player, rows: Sequence[AbilityRow]) -> list[dict[str, Any]]:
    """_display_rows puts rows on the wire for viewer, with each player's name
    and hero as the Tab screen shows them now."""
    # Read the names once, and each hero's class from its id.
    if not rows:
        return []
    names = {connection.player.slot: connection.name for connection in players()}
    encoded: list[dict[str, Any]] = []
    for row in rows:
        pawn = row["player"].pawn()
        hero = None if pawn is None else _HERO_NAMES.get(pawn.hero)
        abilities: list[dict[str, Any]] = []
        for shown in row["abilities"][:4]:
            abilities.append(
                {
                    "ability": _item_id(shown["name"]) if shown and shown["name"] else 0,
                    "image": shown["image"] if shown else "",
                    "upgrades": shown.get("upgrades", 0) if shown else 0,
                }
            )
        encoded.append(
            {
                "player": row["player"].slot,
                # A Steam name is at most 32 characters, within the host's 128
                # bytes.
                "name": names.get(row["player"].slot, "")[:32],
                "hero": hero or "",
                "own": row["player"].slot == viewer.slot,
                "abilities": abilities,
            }
        )
    return encoded


# _HERO_NAMES maps each hero's id to its game name.
_HERO_NAMES = {each.id: each.name for each in HEROES}

# _ABILITY_BAR_NODE matches the node of a click on the game's ability bar.
_ABILITY_BAR_NODE = re.compile(r"ability:([0-3])")


def cursor_free(player: Player) -> bool | None:
    """cursor_free reports whether player's mouse cursor is up now, whether
    the mod freed it with free_cursor or the player did with the game's own
    keys. It is None while the player has no hero to read."""
    pawn = player.pawn()
    if pawn is None:
        return None
    value = read_field(pawn.entity, "CCitadelPlayer_MovementServices", "m_bHasFreeCursor", "bool")
    return value if isinstance(value, bool) else None


def sheet(path: str) -> bool:
    """sheet adds the style sheet at path in the mod's content styles
    directory, such as "hud.css", to every interface the mod shows, after the
    sheets added before it. Adding a sheet again changes nothing. A path is
    letters, digits, "_", "-" and "." in segments separated by "/", ending in
    ".css"; another adds nothing and returns False."""
    # Refuse a path the renderer cannot load, and keep a sheet added before.
    if not path.endswith(".css") or not all(
        _NAME.fullmatch(part) and part != ".." for part in path.split("/")
    ):
        return False
    if path in _sheets:
        return True

    # Redraw every interface with the new sheet.
    _sheets.append(path)
    for view in list(_views.values()):
        _draw(view.player)
    return True


# _NAME matches one segment of a sheet's path, and a route's name.
_NAME = re.compile(r"[\w.-]+", re.ASCII)

# _sheets lists the mod's style sheets, in the order the mod added them.
_sheets: list[str] = []


def route(player: Player, name: str, value: object) -> bool:
    """route sends player value under name, for every layout of the mod to
    read with Modlock.route, until the next call; None removes it. A name is
    letters, digits, "_", "-" and ".", and the value is anything json.dumps
    takes. A route with another name, or a value JSON cannot hold, sends
    nothing and returns False."""
    # Refuse a name no attribute can carry and a value JSON cannot hold.
    encoded = None if value is None else _json(value)
    if not _NAME.fullmatch(name) or (value is not None and encoded is None):
        return False

    # Store the value, or forget the route, and redraw.
    routes = _view(player).routes
    if encoded is None:
        routes.pop(name, None)
    else:
        routes[name] = encoded
    return _draw(player)


def hide(player: Player) -> bool:
    """hide removes the interface, the routes and the notices the mod shows
    player."""
    _views.pop(player.slot, None)
    _screens.pop(player.slot, None)
    return _send(player.slot, {"reset": True})


def toast(player: Player, text: str, seconds: float = 4) -> bool:
    """toast shows player text in a notice at the lower left for seconds.
    Notices stack, newest last, slide in, and fade as they expire."""
    global _next_notice
    view = _view(player)
    view.notices = [*view.notices, _Notice(_next_notice, text, _now + seconds)][-_MAX_NOTICES:]
    _next_notice += 1
    return _draw(player)


def ui_sound(player: Player, sound: str, seconds: float = 0) -> bool:
    """ui_sound plays the game's sound event sound, such as
    "UI.ItemDraft.Start", in player's game alone, once; seconds cuts it after
    that long, keeping only its start. It plays with the next screen the
    player reads, and not at all if the player reads it only after the mod's
    next few sounds."""
    global _next_cue
    view = _view(player)
    view.cues = [*view.cues, {"number": _next_cue, "sound": sound, "seconds": seconds}][-_MAX_CUES:]
    _next_cue += 1
    return _draw(player)


def _press(player: Player, node: str) -> None:
    """_press runs the handler of the button the player pressed."""
    # A click on the game's ability bar names its slot, which no button's id
    # does.
    bar = _ABILITY_BAR_NODE.fullmatch(node)
    if bar:
        view = _views.get(player.slot)
        if view is not None and view.bar is not None:
            view.bar(player, int(bar[1]))
        return
    screen = _screens.get(player.slot)
    pressed = None if screen is None else screen.presses.get(node)
    if pressed is not None:
        pressed(player)


def _change(player: Player, node: str, value: str | float | bool | int | Wheel | None) -> None:
    """_change runs the handler of the input the player changed, with its
    value as the host checked it."""
    screen = _screens.get(player.slot)
    if screen is None:
        return
    if isinstance(value, Wheel):
        wheeled = screen.wheels.get(node)
        if wheeled is not None:
            wheeled(player, value)
        return
    changed = screen.changes.get(node)
    if changed is not None:
        changed(player, value)


def _tick(seconds: float) -> None:
    """_tick advances the clock to the frame at seconds and fades expiring
    notices."""
    global _now
    _now = seconds
    for view in list(_views.values()):
        if not view.notices:
            continue
        view.notices = [notice for notice in view.notices if notice.until > _now]
        _draw(view.player)


@dataclass(frozen=True, slots=True)
class _Notice:
    """_Notice is one toast, shown until the game clock passes until."""

    id: int
    text: str
    until: float


@dataclass(slots=True)
class _View:
    """_View is what the mod shows one player: its interface and its
    notices."""

    player: Player
    root: Child = None
    hud: list[HudPart] = field(default_factory=lambda: list[HudPart]())
    # cursor is true while the mod frees the player's mouse cursor.
    cursor: bool = False
    # asks counts the mod's calls that freed the cursor.
    asks: int = 0
    # routes maps each route's name to its value as JSON.
    routes: dict[str, str] = field(default_factory=lambda: dict[str, str]())
    # cues are the last sounds played, which the renderer plays once each.
    cues: list[dict[str, Any]] = field(default_factory=lambda: list[dict[str, Any]]())
    # bar takes clicks on the game's ability bar, while the mod asks for them.
    bar: AbilityBarHandler | None = None
    # display is the abilities shown on Tab's rows and the ability bar.
    display: list[AbilityRow] = field(default_factory=lambda: list[AbilityRow]())
    notices: list[_Notice] = field(default_factory=lambda: list[_Notice]())


# _views holds what the mod shows each player, by slot.
_views: dict[int, _View] = {}

# _now is the game clock at the last frame, in seconds.
_now = 0.0

# _next_notice numbers the next notice, which keys it in the stack.
_next_notice = 0

# _next_cue numbers the next sound, so the renderer plays each once.
_next_cue = 1

# _MAX_CUES is how many sounds the root keeps, so a player whose screens come
# slower than the mod plays sounds misses none of the last few.
_MAX_CUES = 8

# _MAX_NOTICES bounds the stack; a newer notice pushes out the oldest.
_MAX_NOTICES = 4

# _NOTICE_TRANSITION, _NOTICE_ENTER and _NOTICE_EXIT slide a notice in from
# the left and fade it out when it expires.
_NOTICE_TRANSITION: Transition = {"seconds": 0.25, "easing": "out"}
_NOTICE_ENTER: Style = {"opacity": 0, "translate": (-16, 0)}
_NOTICE_EXIT: Style = {"opacity": 0}


def _view(player: Player) -> _View:
    """_view returns what the mod shows player, made empty the first time."""
    view = _views.get(player.slot)
    if view is None:
        view = _views[player.slot] = _View(player)
    return view


def _draw(player: Player) -> bool:
    """_draw sends what changed in player's interface and notices."""
    # Render the player's view and any notices against the last screen sent.
    view = _view(player)
    root = (view.root, bool(view.notices) and _notices(view.notices))
    previous = _screens.get(player.slot)
    display = _display_rows(player, view.display)

    def render(previous: _Screen | None) -> _Screen:
        return _render(previous, root, view, display)

    following = render(previous)
    change = _diff(previous, following)

    # Send the change; a failed send means the player may have left, so send
    # the screen whole.
    if change is not None and not _send(player.slot, change):
        whole = render(None)
        if not _send(player.slot, _diff(None, whole) or {}):
            _screens.pop(player.slot, None)
            return False
        _screens[player.slot] = whole
        return True

    # Keep the screen the player now holds.
    _screens[player.slot] = following
    return True


def _notices(stack: list[_Notice]) -> Element:
    """_notices stacks the notices at the lower left, newest at the bottom,
    clear of the game's health, weapon and spirit stats and the status icons
    above them."""
    # The stack fades with its last notice.
    return panel(
        style={
            "width": "fit",
            "height": "fit",
            "flow": "down",
            "horizontal_align": "left",
            "vertical_align": "bottom",
            "margin": (0, 0, 260, 24),
            "transition": _NOTICE_TRANSITION,
        },
        exit=_NOTICE_EXIT,
    )[
        [
            label(
                key=notice.id,
                style={
                    "horizontal_align": "left",
                    "margin": (4, 0, 0, 0),
                    "padding": (5, 10),
                    "background": "#101418d8",
                    "color": "#f2f2f2",
                    "font_size": 16,
                    "border_radius": 4,
                    "transition": _NOTICE_TRANSITION,
                },
                enter=_NOTICE_ENTER,
                exit=_NOTICE_EXIT,
            )[notice.text]
            for notice in stack
        ]
    ]


@dataclass(slots=True)
class _Screen:
    """_Screen is what one player's interface holds, as the host last
    accepted it."""

    # nodes maps each node's id to the node and its encoding, to find what
    # changed.
    nodes: dict[str, tuple[dict[str, Any], bytes]] = field(
        default_factory=lambda: dict[str, tuple[dict[str, Any], bytes]]()
    )
    # ids maps each element's place in the tree to its node's id. A place is
    # its parent's place, then "/" for the children, ",index" or ",=key" in a
    # list, and ">" for what a component returned.
    ids: dict[str, str] = field(default_factory=lambda: dict[str, str]())
    # presses maps each button's id to its handler.
    presses: dict[str, Callable[[Player], object]] = field(
        default_factory=lambda: dict[str, Callable[[Player], object]]()
    )
    # changes maps each input's id to its handler.
    changes: dict[str, Callable[[Player, Any], object]] = field(
        default_factory=lambda: dict[str, Callable[[Player, Any], object]]()
    )
    # wheels holds only the enabled sliders that opted in to wheel events.
    wheels: dict[str, Callable[[Player, Wheel], object]] = field(
        default_factory=lambda: dict[str, Callable[[Player, Wheel], object]]()
    )
    # next numbers the next new node, so an id is never reused.
    next: int = 0


# _screens holds each player's interface by slot.
_screens: dict[int, _Screen] = {}


def _render(previous: _Screen | None, root: Child, view: _View, display: list[dict[str, Any]]) -> _Screen:
    """_render lays root out as nodes, keeping the ids of elements that stayed
    in place, beside a node for each route."""
    # The root is the full-screen panel the elements hang from, and it lists
    # the parts of the game's HUD to hide, whether the cursor is free, the
    # mod's sheets and its last sounds.
    layout = _Layout(previous)
    layout.add(
        {
            "id": "",
            "kind": "panel",
            "children": _place(layout, root, "/"),
            "hide_hud": list(view.hud),
            "free_cursor": view.cursor,
            "free_cursor_asks": view.asks,
            "sheets": list(_sheets),
            "cues": list(view.cues),
            "ability_bar_presses": view.bar is not None,
            "ability_display": display,
        }
    )

    # A route's id holds a colon, which no element's id does.
    for name, data in view.routes.items():
        layout.add({"id": f"route:{name}", "kind": "route", "route": name, "data": data})
    return layout.screen


class _Layout:
    """_Layout holds one screen while _place lays it out."""

    __slots__ = ("screen", "_previous")

    def __init__(self, previous: _Screen | None) -> None:
        """The layout continues previous's id counter and keeps its ids."""
        self._previous = previous
        self.screen = _Screen(next=0 if previous is None else previous.next)

    def add(self, node: dict[str, Any]) -> None:
        """add records a node with its encoding."""
        self.screen.nodes[node["id"]] = (node, wire.encode(_SCHEMA, "Node", node))

    def id(self, place: str) -> str:
        """id returns the node id for the element at place."""
        found = None if self._previous is None else self._previous.ids.get(place)
        if found is None:
            found = _base36(self.screen.next)
            self.screen.next += 1
        self.screen.ids[place] = found
        return found


def _place(layout: _Layout, child: Child, at: str) -> list[str]:
    """_place lays out child at place at in layout and returns its nodes'
    ids."""
    # Lay out empty values, lists, text, and the fragments components return.
    if child is None or isinstance(child, bool):
        return []
    if isinstance(child, (str, int, float)):
        node = {"id": layout.id(at), "kind": "label", "text": _js_text(child)}
        layout.add(node)
        return [node["id"]]
    if not isinstance(child, Element):
        ids: list[str] = []
        for index, item in enumerate(child):
            key = item._key if isinstance(item, Element) else None  # pyright: ignore[reportPrivateUsage]
            ids += _place(layout, item, f"{at},{index}" if key is None else f"{at},={key}")
        return ids
    if child._kind == _FRAGMENT:  # pyright: ignore[reportPrivateUsage]
        render = child._render  # pyright: ignore[reportPrivateUsage]
        children = child._children  # pyright: ignore[reportPrivateUsage]
        return _place(layout, children if render is None else render(children), f"{at}>")

    # Draw an intrinsic element.
    props = child._props  # pyright: ignore[reportPrivateUsage]
    kind = child._kind  # pyright: ignore[reportPrivateUsage]
    ability: AbilityTip | None = props.get("ability")
    node: dict[str, Any] = {
        "id": layout.id(at),
        "style": _style(props.get("style")),
        "hover": _style(props.get("hover")),
        "tooltip": props.get("tooltip"),
        "item": _item_id(props["item"]) if props.get("item") else None,
        "ability": _item_id(ability["name"]) if ability and ability["name"] else None,
        "ability_player": ability["player"].slot if ability and ability["name"] else None,
        "hover_sound": props.get("hover_sound"),
        "enter": _style(props.get("enter")),
        "exit": _style(props.get("exit")),
        "cycle": _cycle(props.get("cycle")),
        "classes": _classes(props.get("class_")),
    }
    children = child._children  # pyright: ignore[reportPrivateUsage]
    match kind:
        case "label":
            node["kind"] = "label"
            node["text"] = _text(children)
            node["token"] = props.get("token")
            node["variables"] = _variables(props.get("variables"))
        case "avatar":
            node["kind"] = "avatar"
            node["steam_id"] = props.get("steam_id")
        case "entry" | "slider" | "toggle" | "dropdown":
            _input(layout, node, kind, props, children)
        case "image":
            node["kind"] = "image"
            node["image"] = props.get("src", "")
            node["scaling"] = props.get("scaling")
        case "hero":
            node["kind"] = "hero_image"
            node["hero"] = props.get("hero")
            node["hero_look"] = props.get("look")
            node["scaling"] = props.get("scaling")
        case "button":
            node["kind"] = "button"
            node["sound"] = props.get("sound")
            node["disabled"] = props.get("disabled")
            # A disabled button's press runs nothing, whoever sends it.
            on_press = props.get("on_press")
            if on_press is not None and not props.get("disabled"):
                layout.screen.presses[node["id"]] = on_press
            node["children"] = _place(layout, children, f"{at}/")
        case "layout":
            node["kind"] = "layout"
            node["layout"] = props.get("src", "")
            data = props.get("data")
            node["data"] = "" if data is None else (_json(data) or "")
        case _:
            node["kind"] = "panel"
            node["children"] = _place(layout, children, f"{at}/")
    layout.add(node)
    return [node["id"]]


def _classes(names: str | None) -> list[str] | None:
    """_classes splits an element's class_ into its names, or None for
    none."""
    found = [name for name in (names or "").split(" ") if name != ""]
    return found or None


def _input(layout: _Layout, node: dict[str, Any], kind: str, props: Mapping[str, Any], children: Child) -> None:
    """_input lays out an input of kind from props in node. The node holds
    its control under the case's own name, which the encoder needs because
    every case is a message."""
    # A disabled input's change runs nothing, whoever sends it.
    node["kind"] = "input"
    disabled = props.get("disabled")
    node["disabled"] = disabled
    on_change = props.get("on_change")
    if on_change is not None and not disabled:
        layout.screen.changes[node["id"]] = on_change

    # Each control carries its value and its bounds.
    match kind:
        case "entry":
            node["text_entry"] = {
                "text": str(props.get("value") or ""),
                "placeholder": props.get("placeholder"),
                "max_length": props.get("max_length"),
            }
        case "slider":
            on_wheel = props.get("on_wheel")
            if on_wheel is not None and not disabled:
                layout.screen.wheels[node["id"]] = on_wheel
            node["slider"] = {
                "value": float(props.get("value") or 0),
                "min": props.get("min"),
                "max": props.get("max"),
                "step": props.get("step"),
                "wheel": on_wheel is not None and not disabled,
            }
        case "toggle":
            node["text"] = _text(children)
            node["toggle"] = {"on": props.get("on")}
        case _:
            node["dropdown"] = {"options": list(props.get("options", ())), "selected": props.get("selected")}


def _variables(value: Mapping[str, str | float] | None) -> list[dict[str, str]] | None:
    """_variables encodes a localized label's variables as text, one map
    entry each."""
    if not value:
        return None
    return [{"key": name, "value": _js_text(filled)} for name, filled in value.items()]


def _cycle(value: Cycle | None) -> dict[str, Any] | None:
    """_cycle encodes a cycle."""
    if not value:
        return None
    return {"style": _style(value["style"]), "seconds": value["seconds"], "count": value.get("count")}


def _diff(previous: _Screen | None, following: _Screen) -> dict[str, Any] | None:
    """_diff returns the change from previous to following, or None when
    nothing changed."""
    before = {} if previous is None else previous.nodes
    changed = [node for id, (node, encoded) in following.nodes.items() if id not in before or before[id][1] != encoded]
    removed = [id for id in before if id not in following.nodes]
    if previous is not None and not changed and not removed:
        return None
    return {"reset": previous is None, "set": changed, "removed": removed}


def _send(slot: int, change: dict[str, Any]) -> bool:
    """_send asks the host to apply change to the player in slot."""
    return _call("Ui", "UiRequest", {"player": slot, "change": change}) is not None


def _text(children: Child) -> str:
    """_text joins a label's text children."""
    if children is None or isinstance(children, (bool, Element)):
        return ""
    if isinstance(children, (str, int, float)):
        return _js_text(children)
    return "".join(_text(child) for child in children)


def _js_text(value: str | float) -> str:
    """_js_text writes value as JavaScript's String does, so 3.0 reads 3."""
    if isinstance(value, float) and value.is_integer() and abs(value) < 1e21:
        return str(int(value))
    return str(value)


def _json(value: object) -> str | None:
    """_json encodes value as JSON.stringify does, or returns None for a value
    JSON cannot hold."""
    try:
        return json.dumps(value, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    except (TypeError, ValueError):
        return None


def _base36(number: int) -> str:
    """_base36 writes number in base 36, as JavaScript's toString(36)."""
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    out = ""
    while True:
        number, digit = divmod(number, 36)
        out = digits[digit] + out
        if number == 0:
            return out


def _item_id(name: str) -> int:
    """_item_id names an item or ability the way the game does: the string
    token of its class name, MurmurHash2 of the name as written with the
    token seed 0x31415926."""
    # Seed the hash with the name's length.
    m = 0x5BD1E995
    data = name.encode()
    h = (0x31415926 ^ len(data)) & 0xFFFFFFFF
    i = 0

    # Mix in every whole four bytes.
    while len(data) - i >= 4:
        k = int.from_bytes(data[i : i + 4], "little")
        k = k * m & 0xFFFFFFFF
        k ^= k >> 24
        k = k * m & 0xFFFFFFFF
        h = h * m & 0xFFFFFFFF
        h ^= k
        i += 4

    # Mix in the last one to three bytes, then finish.
    rest = len(data) - i
    if rest >= 3:
        h ^= data[i + 2] << 16
    if rest >= 2:
        h ^= data[i + 1] << 8
    if rest >= 1:
        h ^= data[i]
        h = h * m & 0xFFFFFFFF
    h ^= h >> 13
    h = h * m & 0xFFFFFFFF
    return h ^ h >> 15


# _ALIGNS maps each alignment to its wire value.
_ALIGNS = {"left": "start", "top": "start", "center": "center", "right": "end", "bottom": "end"}

# _EASINGS maps each easing to its wire value.
_EASINGS = {"out": "ease_out", "in": "ease_in", "in_out": "ease_in_out", "linear": "linear"}


def _style(style: Style | None) -> dict[str, Any] | None:
    """_style encodes style. Its other fields name their wire values as they
    are."""
    if style is None:
        return None
    overflow = style.get("overflow")
    return {
        "width": _length(style.get("width")),
        "height": _length(style.get("height")),
        "flow": style.get("flow"),
        "horizontal_align": _align(style.get("horizontal_align")),
        "vertical_align": _align(style.get("vertical_align")),
        "margin": _edges(style.get("margin")),
        "padding": _edges(style.get("padding")),
        "background": _color(style.get("background")),
        "color": _color(style.get("color")),
        "font_size": style.get("font_size"),
        "bold": style.get("bold"),
        "text_align": _align(style.get("text_align")),
        "border_radius": style.get("border_radius"),
        "opacity": style.get("opacity"),
        "face": style.get("face"),
        "uppercase": style.get("uppercase"),
        "letter_spacing": style.get("letter_spacing"),
        "shadow": style.get("shadow"),
        "border_width": style.get("border_width"),
        "border_color": _color(style.get("border_color")),
        "x": _length(style.get("x")),
        "y": _length(style.get("y")),
        "min_width": _length(style.get("min_width")),
        "max_width": _length(style.get("max_width")),
        "min_height": _length(style.get("min_height")),
        "max_height": _length(style.get("max_height")),
        "z_index": style.get("z_index"),
        "overflow_x": style.get("overflow_x", overflow),
        "overflow_y": style.get("overflow_y", overflow),
        "gradient": _gradient(style.get("gradient")),
        "box_shadow": _shadow(style.get("box_shadow")),
        "text_shadow": _shadow(style.get("text_shadow")),
        "tint": _color(style.get("tint")),
        "saturation": style.get("saturation"),
        "brightness": style.get("brightness"),
        "contrast": style.get("contrast"),
        "blur": style.get("blur"),
        "italic": style.get("italic"),
        "weight": style.get("weight"),
        "line_height": style.get("line_height"),
        "no_wrap": style.get("no_wrap"),
        "text_overflow": style.get("text_overflow"),
        "decoration": style.get("decoration"),
        "transform": _transform(style),
        "transition": _transition(style.get("transition")),
    }


def _align(value: str | None) -> str | None:
    """_align encodes an alignment."""
    return None if value is None else _ALIGNS[value]


def _gradient(value: Gradient | None) -> dict[str, Any] | None:
    """_gradient encodes a gradient, spacing colors without places evenly."""
    if value is None:
        return None
    colors = value["colors"]
    at = value.get("at", ())
    last = max(1, len(colors) - 1)
    return {
        "shape": value.get("shape"),
        "stops": [
            {"at": at[index] if index < len(at) else index / last, "color": _color(each)}
            for index, each in enumerate(colors)
        ],
    }


def _shadow(value: Shadow | None) -> dict[str, Any] | None:
    """_shadow encodes a shadow."""
    if value is None:
        return None
    return {
        "x": value.get("x"),
        "y": value.get("y"),
        "blur": value.get("blur"),
        "spread": value.get("spread"),
        "color": _color(value["color"]),
        "inset": value.get("inset"),
    }


def _transform(style: Style) -> dict[str, Any] | None:
    """_transform encodes a style's move, scale, turn and origin, or none."""
    translate = style.get("translate")
    scale = style.get("scale")
    rotate = style.get("rotate")
    origin = style.get("origin")
    if translate is None and scale is None and rotate is None and origin is None:
        return None
    scales = (scale, scale) if isinstance(scale, (int, float)) else scale
    return {
        "translate_x": None if translate is None else translate[0],
        "translate_y": None if translate is None else translate[1],
        "scale_x": None if scales is None else scales[0],
        "scale_y": None if scales is None else scales[1],
        "rotate": rotate,
        "origin_x": None if origin is None else origin[0],
        "origin_y": None if origin is None else origin[1],
    }


def _transition(value: float | Transition | None) -> dict[str, Any] | None:
    """_transition encodes a transition: seconds, or a Transition."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return {"seconds": value}
    easing = value.get("easing")
    return {
        "seconds": value["seconds"],
        "delay": value.get("delay"),
        "easing": None if easing is None else _EASINGS[easing],
    }


def _length(value: Length | None) -> dict[str, Any] | None:
    """_length encodes a length as its case of the wire's Length."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return {"pixels": value}
    if value == "fit":
        return {"fit_children": True}
    if value == "fill":
        return {"fill": 1}
    return {"percent": float(value.rstrip("%"))}


def _edges(value: Edges | None) -> dict[str, float] | None:
    """_edges encodes edges."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return {"top": value, "right": value, "bottom": value, "left": value}
    if len(value) == 2:
        return {"top": value[0], "right": value[1], "bottom": value[0], "left": value[1]}
    return {"top": value[0], "right": value[1], "bottom": value[2], "left": value[3]}


# _HEX matches a color's hex digits.
_HEX = re.compile(r"[0-9a-fA-F]{6}([0-9a-fA-F]{2})?")


def _color(value: str | None) -> int | None:
    """_color encodes "#rrggbb" or "#rrggbbaa" as 0xRRGGBBAA."""
    digits = None if value is None else value.removeprefix("#")
    if not digits or not _HEX.fullmatch(digits):
        return None
    return int(digits + "ff" if len(digits) == 6 else digits, 16)


_serve_ui(_UiHandlers(press=_press, change=_change, tick=_tick))
