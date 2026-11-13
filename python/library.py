# The modlock library writes Modlock mods in Python. A mod registers its
# handlers when its main module runs; modlock-host runs the mod in a sandbox
# inside the game server and calls the handlers on the server's frame
# thread, one at a time.
#
#   import modlock
#
#   @modlock.command("hello")
#   def hello(player: modlock.Player, args: str) -> None:
#       player.chat("Hello from Python!")
#
# Build it with modlock build.
#
# The calls into the game and their types above are generated from the
# schema. A call that fails logs why in the server log and returns False or
# None, so a mod can retry on a later frame instead of stopping. print writes
# to the server log.

import io
import math
import random
import sys
import traceback

from .heroes import HEROES as HEROES
from .heroes import Hero as Hero
from .heroes import find_hero as find_hero
from .heroes import hero_named as hero_named


class Buttons(enum.IntFlag):
    """Buttons names input bits for block_input, watch_input and
    Player.press."""

    # ATTACK is the primary fire button.
    ATTACK = 1
    # JUMP is the jump key, Space by default (IN_MANTLE). Blocking it stops
    # the native jump and mantle; the jumping client still predicts the jump
    # and is corrected.
    JUMP = 1 << 48
    # CROUCH is the crouch key, Ctrl by default.
    CROUCH = 1 << 2
    # DASH is the dash key, Shift by default.
    DASH = 1 << 16
    # USE is the interact key.
    USE = 1 << 5
    # ALT_FIRE is the zoom and alternate fire button, right mouse by default.
    ALT_FIRE = 1 << 11
    # RELOAD is the reload key.
    RELOAD = 1 << 13
    # MELEE is the light melee key, Q by default.
    MELEE = 1 << 32
    # ABILITY1 to ABILITY3 are the hero's first three ability keys.
    ABILITY1 = 1 << 33
    ABILITY2 = 1 << 34
    ABILITY3 = 1 << 35
    # ULTIMATE is the hero's ultimate ability key, the fourth.
    ULTIMATE = 1 << 36
    # PARRY is the melee parry button.
    PARRY = 1 << 42


class Layers(enum.IntFlag):
    """Layers names collision layers for trace, as bits."""

    # SOLID is the world and solid objects.
    SOLID = 1
    # HITBOX is the hit boxes of heroes and units.
    HITBOX = 1 << 1
    # TRIGGER is invisible volumes that react to touch.
    TRIGGER = 1 << 2
    # PLAYER_CLIP is walls that only stop heroes.
    PLAYER_CLIP = 1 << 4
    # PLAYER is player-controlled heroes.
    PLAYER = 1 << 18
    # NPC is units that are not players.
    NPC = 1 << 19
    # STATIC_LEVEL is the map's built geometry, foliage included.
    STATIC_LEVEL = 1 << 30
    # HERO, TROOPER and BUILDING are those units' bodies.
    HERO = 1 << 37
    TROOPER = 1 << 38
    BUILDING = 1 << 40


# Service answers the host's calls to one service the mod serves: the
# method's name and payload in, the answer out. An exception becomes the
# call's error.
type Service = Callable[[str, bytes], bytes]

# _commands maps each registered command name to its handler.
_commands: dict[str, Callable[[Player, str], object]] = {}

# _services maps each service the mod serves to its handler.
_services: dict[str, Service] = {}

# Each list runs in registration order.
_starts: list[Callable[[list[str]], object]] = []
_frames: list[Callable[[FrameEvent], object]] = []
_chats: list[Callable[[ChatEvent], object]] = []
_joins: list[Callable[[PlayerJoinedEvent], object]] = []
_leaves: list[Callable[[PlayerLeftEvent], object]] = []
_worlds: list[Callable[[str], object]] = []
_inputs: list[Callable[[Player, Buttons, Buttons], object]] = []
_restoreds: list[Callable[[Player, str | None], object]] = []
_npcs_restoreds: list[Callable[[str | None], object]] = []
_damages: list[Callable[[DamageEvent], DamageResult | None]] = []
_damageds: list[Callable[[DamagedEvent], object]] = []
_launches: list[Callable[[LaunchEvent], object]] = []
_impacts: list[Callable[[ImpactEvent], object]] = []
_landeds: list[Callable[[LandedEvent], object]] = []
_setting_changes: list[Callable[[Player, str, str], object]] = []

_spawns: list[Callable[[Player, Pawn], object]] = []

# _spawned maps each slot whose hero is in play to that hero's entity, as the
# spawn handlers last heard of it.
_spawned: dict[int, int] = {}

# _roster is the player list the host last reported, or None before one.
_roster: list[Connection] | None = None


# _MAX_SLOT is above every player slot and below every Steam ID.
_MAX_SLOT = 0xFFFF


def connection(player: Player | int) -> Connection | None:
    """connection returns the connection of one player, given the player,
    their slot or their Steam ID: their name, Steam ID, whether they are a bot
    and whether they finished joining. A number above 65535 is a Steam ID. It
    returns None when nobody holds the slot or the account is not connected.
    Its player field is the Player to call."""
    held = _roster if _roster is not None else players()
    if isinstance(player, int) and player > _MAX_SLOT:
        return next((c for c in held if c.steam_id == player), None)
    slot = player if isinstance(player, int) else player.slot
    return next((c for c in held if c.player.slot == slot), None)


def players() -> list[Connection]:
    """players lists the occupied player slots in slot order. It answers from
    the roster the host last reported, and asks the host until a report
    arrives."""
    if _roster is not None:
        return list(_roster)
    response = _call("Players", "Empty", {}, "PlayersResponse")
    if response is None:
        return []
    return response["players"]


# _player_watchers hear each player list the host reports.
_player_watchers: list[Callable[[list[Connection]], object]] = []


def _watch_players(watcher: Callable[[list[Connection]], object]) -> None:
    """_watch_players calls watcher with each player list the host reports,
    and at once with the last one when a report already arrived."""
    _player_watchers.append(watcher)
    if _roster is not None:
        watcher(_roster)


# Cast keeps a mode's players: one member for each human who finished
# joining, made by the mode's join and ended by its leave. Members are keyed
# by slot and connection generation, so a reconnect is a leave and a join.
# Cast learns of joins and leaves from the host's player reports and settles
# them on the next frame; it never walks the player list on a frame where
# nothing changed.
#
# A member may carry a want: to sit on a hero and team, and to come back a
# while after its hero falls. The mode gives one want for every member or a
# function that reads it from the member each frame, so a want follows the
# member's own state. A human whose want leaves the hero free still gets one
# when the game leaves them on a team without a hero, as its own spawn on
# joining can, since nothing else would ever seat them. Each frame Cast
# compares the want with what the member's hero shows, asks the game again at
# most once a second while they differ, and stops asking once they match.
# That is a reconciliation loop: a lost or refused ask is only asked again,
# never reported as a failure.
#
# A frame costs one pawn read per member, which the mode reads back with
# Cast.pawn instead of asking the game again.


@dataclasses.dataclass(frozen=True, slots=True, kw_only=True)
class Want:
    """Want is what a member's hero should be; each field it leaves None is
    free."""

    # hero seats the member on a hero: its name as players know it or as the
    # game does, such as "Calico" or "hero_nano", its id, or "any" for the
    # hero they have or else a random one.
    hero: str | int | None = None
    # team is the team, 2 or 3, to seat the member on. Without one a member
    # keeps their hero's team, or else joins the side with fewer members.
    team: int | None = None
    # respawn is the seconds a fallen hero waits before Cast asks for it back.
    respawn: float | None = None


# _CHANGE_SECONDS is how long Cast leaves a member without a hero after they
# had one, or after they joined when their hero is free. The game's own
# Change Hero removes the hero before it spawns the new one, and seating the
# member in that gap would override their pick.
_CHANGE_SECONDS = 3.0

# _NO_WANT leaves a member's hero free.
_NO_WANT = Want()


@dataclasses.dataclass(slots=True)
class _Seat[M]:
    """_Seat is one member with what Cast last saw of their hero."""

    connection: Connection
    member: M
    # joined_at is when the member joined.
    joined_at: float
    # pawn is the member's hero this frame.
    pawn: Pawn | None = None
    alive: bool = False
    # fell_at is when Cast last saw the hero fall.
    fell_at: float = -math.inf
    # hero is the member's hero last frame, or 0 without one.
    hero: int = 0
    # hero_lost_at is when the member last went from a hero to none.
    hero_lost_at: float = -math.inf
    # asked_at is when Cast last asked the game for the want.
    asked_at: float = -math.inf


class Cast[M]:
    """Cast keeps one member of type M per human player.

    join makes the member for a human who finished joining, and leave ends a
    member whose connection left the slot. want is what every member wants,
    or a function that returns what one member wants now; Cast calls it once
    per member each frame, so it should return a want it keeps rather than
    make a new one. bots says what to do with a bot: "skip" leaves it out of
    the cast, "join" makes it a member as a human is, and "kick" asks the
    server to remove it, once per connection."""

    def __init__(
        self,
        join: Callable[[Connection], M],
        *,
        leave: Callable[[M, Connection], object] | None = None,
        want: Want | Callable[[M], Want] = _NO_WANT,
        bots: Literal["skip", "join", "kick"] = "skip",
    ) -> None:
        self._join = join
        self._leave = leave
        self._want = want
        self._bots = bots
        # _seats holds each member by slot, in the order they joined.
        self._seats: dict[int, _Seat[M]] = {}
        # _kicked holds the connection generation each kicked bot's slot had.
        self._kicked: dict[int, int] = {}
        # _reported is the player list the host reported since the last frame.
        self._reported: list[Connection] | None = None
        # _random draws the hero for "any". It is the cast's own, seeded when
        # the mod makes the cast, because the runtime's shared generator
        # starts from the same seed in every instance.
        self._random = random.Random()
        _watch_players(self._report)

    @property
    def size(self) -> int:
        """size is the number of members."""
        return len(self._seats)

    def members(self) -> list[M]:
        """members lists the members in the order they joined."""
        return [seat.member for seat in self._seats.values()]

    def get(self, player: Player) -> M | None:
        """get returns the member in player's slot, if any."""
        seat = self._seats.get(player.slot)
        return None if seat is None else seat.member

    def pawn(self, player: Player) -> Pawn | None:
        """pawn returns the member's hero as Cast read it this frame."""
        seat = self._seats.get(player.slot)
        return None if seat is None else seat.pawn

    def frame(self, seconds: float) -> None:
        """frame settles the joins and leaves reported since the last frame,
        reads each member's hero, and asks the game once a second for any want
        it does not show yet. seconds is the game clock."""
        if self._reported is not None:
            self._settle(self._reported, seconds)
            self._reported = None
        for seat in self._seats.values():
            self._reconcile(seat, seconds)

    def _report(self, players: list[Connection]) -> None:
        self._reported = players

    def _settle(self, players: list[Connection], now: float) -> None:
        """_settle ends the members whose connection left and makes the new
        ones, joined at now."""
        # A member leaves when their slot empties or a new connection takes it.
        present = {c.player.slot: c for c in players}
        for slot, seat in list(self._seats.items()):
            connection = present.get(slot)
            if connection is None or connection.generation != seat.connection.generation:
                del self._seats[slot]
                if self._leave is not None:
                    self._leave(seat.member, seat.connection)

        # A human who finished joining becomes a member, and a bot as the
        # policy says.
        for connection in players:
            slot = connection.player.slot
            if not connection.ready or slot in self._seats:
                continue
            if connection.bot and self._bots != "join":
                if self._bots == "kick":
                    self._kick(connection)
                continue
            self._seats[slot] = _Seat(connection, self._join(connection), now)

    def _kick(self, connection: Connection) -> None:
        """_kick asks the server to remove a bot, once per connection."""
        slot = connection.player.slot
        if self._kicked.get(slot) == connection.generation:
            return
        self._kicked[slot] = connection.generation
        server_command(f'kick "{connection.name}"')

    def _reconcile(self, seat: _Seat[M], now: float) -> None:
        """_reconcile reads a member's hero and asks for the part of the want
        it lacks."""
        # Note when the hero falls.
        pawn = seat.connection.player.pawn()
        alive = pawn is not None and pawn.hero != 0 and pawn.health > 0
        if seat.alive and not alive:
            seat.fell_at = now
        seat.pawn = pawn
        seat.alive = alive

        # Note when a member who had a hero is left without one, so a hero
        # change the game is making finishes before Cast seats them again.
        had = 0 if pawn is None else pawn.hero
        if had == 0 and seat.hero != 0:
            seat.hero_lost_at = now
        seat.hero = had
        changing = had == 0 and now - seat.hero_lost_at < _CHANGE_SECONDS

        # Ask at most once a second, first for the seat, then for the hero
        # back.
        if now - seat.asked_at < 1 or changing:
            return
        want = self._want if isinstance(self._want, Want) else self._want(seat.member)
        hero = want.hero if want.hero is not None else self._stranded(seat, now)
        if hero is not None and not self._seated(seat, want, hero):
            seat.asked_at = now
            self._seat(seat, want, hero)
        elif (
            want.respawn is not None
            and pawn is not None
            and pawn.hero != 0
            and not alive
            and now - seat.fell_at >= want.respawn
        ):
            seat.asked_at = now
            seat.connection.player.respawn()

    def _stranded(self, seat: _Seat[M], now: float) -> Literal["any"] | None:
        """_stranded returns "any" for a human whose hero is free when the
        game has left them on a team without one for _CHANGE_SECONDS since
        they joined, and otherwise leaves the hero free. Such a pawn never
        gets a hero on its own, while a player still choosing one has no pawn
        and a spectator stands on team 1. A bot is left to the game, which
        seats it."""
        pawn = seat.pawn
        heroless = pawn is not None and pawn.hero == 0 and pawn.team in (2, 3)
        if heroless and not seat.connection.bot and now - seat.joined_at >= _CHANGE_SECONDS:
            return "any"
        return None

    def _seated(self, seat: _Seat[M], want: Want, hero: str | int) -> bool:
        """_seated reports whether the member's hero shows the hero and team
        they want."""
        # A member needs a hero on the team they want before the hero counts.
        pawn = seat.pawn
        if pawn is None or pawn.hero == 0:
            return False
        if want.team is not None and pawn.team != want.team:
            return False
        if hero == "any":
            return True

        # A name outside the released heroes, such as one the game added
        # since, is met by whatever hero the game gave for it.
        if isinstance(hero, int):
            return pawn.hero == hero
        known = hero_named(hero)
        return known is None or pawn.hero == known.id

    def _seat(self, seat: _Seat[M], want: Want, hero: str | int) -> None:
        """_seat asks the game to put the member on hero and their team."""
        # Any hero keeps the one the member has, or else takes a random one.
        pawn = seat.pawn
        pick = hero
        if hero == "any":
            pick = pawn.hero if pawn is not None and pawn.hero != 0 else self._random.choice(HEROES).name
        team = want.team if want.team is not None else self._side(seat)
        seat.connection.player.select_hero(pick, team)

    def _side(self, seat: _Seat[M]) -> int:
        """_side is the team a member joins: their hero's, or the one with
        fewer heroes."""
        # A hero keeps its team. The game puts each connecting player on a
        # team before they have a hero, so a member without one is counted
        # afresh.
        pawn = seat.pawn
        if pawn is not None and pawn.hero != 0 and pawn.team in (2, 3):
            return pawn.team
        amber = sapphire = 0
        for other in self._seats.values():
            if other is seat or other.pawn is None or other.pawn.hero == 0:
                continue
            if other.pawn.team == 2:
                amber += 1
            if other.pawn.team == 3:
                sapphire += 1
        return 3 if sapphire < amber else 2



# Seating keeps each player the mod seats on the hero and team it asked for.
# seat records what one player should have, and one Cast the library runs
# itself asks the game until the player's hero shows it, and again after a
# hero change. The same Cast brings fallen heroes back after the respawn
# delay. It starts with the first seat or respawn delay, so a mod that uses
# neither pays nothing.

# _seated holds what each seated player wants by slot, with the generation of
# the connection it was asked for, so a wish never passes to the next player
# in the slot.
_seated: dict[int, tuple[int, Want]] = {}

# _unseated is the want of a player the mod did not seat: only the respawn
# delay.
_unseated: Want | None = None

# _seating is the Cast that seats players, once the mod uses seating.
_seating: Cast[Connection] | None = None


def _seat_want(member: Connection) -> Want:
    """_seat_want returns what the seating Cast's member wants now."""
    held = _seated.get(member.player.slot)
    if held is not None and held[0] == member.generation:
        return held[1]
    return _unseated if _unseated is not None else _NO_WANT


def _seat_left(member: Connection, connection: Connection) -> None:
    """_seat_left forgets the wish of a seated player who left."""
    held = _seated.get(member.player.slot)
    if held is not None and held[0] == member.generation:
        del _seated[member.player.slot]


def _start_seating() -> None:
    """_start_seating makes the seating Cast and runs it with the mod's
    frames. Bots are members, so the mod can seat one and the respawn delay
    brings theirs back too."""
    global _seating
    if _seating is not None:
        return
    cast = Cast[Connection](lambda connection: connection, leave=_seat_left, want=_seat_want, bots="join")
    _seating = cast
    _frames.append(lambda event: cast.frame(event.time_seconds))


class _PlayerMethods:
    def connection(self: Player) -> Connection | None:
        """connection returns the player's connection: their name, Steam ID,
        whether they are a bot and whether they finished joining. It returns
        None when nobody holds the slot."""
        return connection(self)

    def seat(self: Player, *, hero: str | int = "any", team: int | None = None) -> bool:
        """seat gives the player a hero on a team, skipping the game's team
        and hero screens. Call it as the player joins:

            @modlock.on_player_join
            def joined(event: modlock.PlayerJoinedEvent) -> None:
                event.connection.player.seat(hero="haze", team=2)

        hero is the hero's name as players know it or as the game does, such
        as "Calico" or "hero_nano", its id, or "any" for the hero the player
        has or else a random one. team is 2 or 3; without one the player
        keeps their hero's team or joins the side with fewer heroes.

        The hero appears on a later frame, and on_spawn hears of it. The
        library asks the game again until the player has the seat, and once
        more after the player changes hero, so a call before the game is
        ready is not lost. Seating the player again replaces the seat, and it
        ends when they leave. It returns False when nobody holds the player's
        slot."""
        held = connection(self)
        if held is None:
            return False
        respawn = None if _unseated is None else _unseated.respawn
        _seated[self.slot] = (held.generation, Want(hero=hero, team=team, respawn=respawn))
        _start_seating()
        return True


def set_respawn_delay(seconds: float | None) -> None:
    """set_respawn_delay brings every fallen hero back after seconds, in
    place of the game's own respawn time. None leaves fallen heroes to the
    game again."""
    global _unseated
    _unseated = None if seconds is None else Want(respawn=seconds)
    for slot, (generation, want) in _seated.items():
        _seated[slot] = (generation, dataclasses.replace(want, respawn=seconds))
    _start_seating()


def set_orbit_camera(player: Player, enabled: bool) -> bool:
    """set_orbit_camera gives the player's hero the hideout's orbit camera,
    zoomed with the wheel, or returns the normal camera. The hideout HUD
    replaces the game HUD while it is on. It returns False when the player has
    no hero or a call fails.

    The pawn's networked m_bInHideoutZone moves its client to the hideout
    view, and the wheel zooms only while the pawn holds
    MODIFIER_STATE_HIDEOUT_ZONE, since the server ignores orbit_camera_in and
    orbit_camera_out without it."""
    pawn = player.pawn()
    if pawn is None or pawn.entity == 0:
        return False
    flag = write_field(pawn.entity, "CCitadelPlayerPawn", "m_bInHideoutZone", "bool", enabled)
    zoom = hold_modifier_state(pawn.entity, "MODIFIER_STATE_HIDEOUT_ZONE", enabled)
    return flag and zoom


# Scores report a player's results to the boards a mod declares in mod.json
# and read those boards back, over the host's scores service. A host without
# the service, such as a server with no backend, takes no score; the mod logs
# each one instead, so its author sees what it would report.
#
#   modlock.report_score(player, "trial.time", elapsed_ms, partition={"course": "canyon"}, key=run_id)

# _SCORES names the host service that keeps scores.
_SCORES = "scores"

# BoardWindow is a span of time a board keeps standings for, in UTC; a week
# starts Monday.
type BoardWindow = Literal["all_time", "day", "week"]


@dataclasses.dataclass(frozen=True, slots=True, kw_only=True)
class MatchPlace:
    """MatchPlace is one player's finishing place in a match, 1 for the
    winner."""

    player: Player
    place: int
    # value is the player's score in the match, for a board that keeps one.
    value: float = 0


@dataclasses.dataclass(frozen=True, slots=True, kw_only=True)
class BoardRow:
    """BoardRow is one player's place on a board."""

    rank: int
    steam_id: int
    name: str
    value: int
    tiebreak: int
    count: int


@dataclasses.dataclass(frozen=True, slots=True, kw_only=True)
class BoardPage:
    """BoardPage is a page of a board."""

    # rows are the standings in rank order.
    rows: list[BoardRow]
    # player is the asked player's row, when they hold a standing.
    player: BoardRow | None
    # loading is set until the host has read the board; read again on a later
    # frame.
    loading: bool


def report_score(
    player: Player,
    board: str,
    value: float,
    *,
    partition: Mapping[str, str] | None = None,
    tiebreak: float = 0,
    key: str | None = None,
    earned_at: float = 0,
    evidence: str | None = None,
) -> bool:
    """report_score reports one score for player to a board the manifest
    declares. It reports whether the host took it; a host without the scores
    service takes none, and the mod logs the score instead.

    partition names a value for each of the board's partitions. tiebreak
    ranks equal values, lower first, such as fewer deaths. key identifies the
    run, so a resent score counts once. earned_at is when the score was
    earned, in Unix milliseconds; the host's clock by default. evidence names
    what shows the run, such as a recording."""
    report = wire.encode(
        _SCHEMA,
        "ScoreReport",
        {
            "board": board,
            "player": player,
            "value": _int64(value),
            "tiebreak": _int64(tiebreak),
            "partition": partition,
            "key": key,
            "earned_at_unix_millis": _int64(earned_at),
            "evidence": evidence,
        },
    )
    return _taken("Report", report, f"{board}{_describe(partition)}: player {player.slot} {value}")


def report_match(
    board: str,
    places: list[MatchPlace],
    *,
    partition: Mapping[str, str] | None = None,
    key: str | None = None,
    earned_at: float = 0,
    evidence: str | None = None,
) -> bool:
    """report_match reports one match's places to a rating or match board the
    manifest declares. Players who tie, such as one team's members, share a
    place. It reports whether the host took it, as report_score does, and
    takes the same options but tiebreak."""
    report = wire.encode(
        _SCHEMA,
        "MatchReport",
        {
            "board": board,
            "places": [{"player": p.player, "place": p.place, "value": _int64(p.value)} for p in places],
            "partition": partition,
            "key": key,
            "earned_at_unix_millis": _int64(earned_at),
            "evidence": evidence,
        },
    )
    line = ", ".join(f"player {p.player.slot} #{p.place}" for p in places)
    return _taken("Match", report, f"{board}{_describe(partition)}: {line}")


def read_board(
    board: str,
    *,
    partition: Mapping[str, str] | None = None,
    window: BoardWindow | None = None,
    offset: int = 0,
    limit: int = 0,
    player: Player | None = None,
) -> BoardPage | None:
    """read_board reads a page of a board the manifest declares, as the host
    last read it, or returns None when the host serves no scores. A page the
    host has not read yet comes back loading.

    partition names a value for each of the board's partitions, window is one
    of the board's windows, all time by default, offset skips that many rows
    from the top and limit bounds the rows, ten by default. player asks for
    this player's own row too."""
    # Ask the host for the page; a host without scores answers nothing.
    query = wire.encode(
        _SCHEMA,
        "BoardQuery",
        {"board": board, "partition": partition, "window": window, "offset": offset, "limit": limit, "player": player},
    )
    reply = call_service(_SCORES, "Read", query)
    if reply is None:
        return None

    # Read the standings into rows.
    page = wire.decode(_SCHEMA, "BoardPage", reply)
    standing = page["player"]
    return BoardPage(
        rows=[BoardRow(**row) for row in page["rows"]],
        player=BoardRow(**standing) if standing is not None else None,
        loading=page["loading"],
    )


def _taken(method: str, report: bytes, line: str) -> bool:
    """_taken calls a reporting method and logs line when the host takes no
    score."""
    if call_service(_SCORES, method, report) is not None:
        return True
    log(f"score {line} (not reported: the host keeps no scores)")
    return False


def _int64(value: float) -> int:
    """_int64 converts a number to the int64 a report carries, rounding a
    fraction half up."""
    return math.floor(value + 0.5)


def _describe(partition: Mapping[str, str] | None) -> str:
    """_describe writes partition values as "[course=canyon]", in name
    order."""
    if not partition:
        return ""
    return "[" + " ".join(f"{name}={partition[name]}" for name in sorted(partition)) + "]"

def serve(service: str) -> Callable[[Service], Service]:
    """serve answers the host's calls to service with the decorated handler,
    replacing its earlier handler."""

    def register(handler: Service) -> Service:
        _services[service] = handler
        return handler

    return register


def command[F: Callable[[Player, str], object]](name: str) -> Callable[[F], F]:
    """command calls the decorated handler when a player types /name in chat
    or their client sends the console command name to the server. The
    handler runs with the next server frame, before the frame handlers, and
    receives the text after the name, trimmed of surrounding spaces.
    Registering a name again replaces its handler.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.command("hello")
        def hello(player: modlock.Player, args: str) -> None: ...
    """

    def register(handler: F) -> F:
        _commands[name] = handler
        return handler

    return register


def on_start[F: Callable[[list[str]], object]](handler: F) -> F:
    """on_start calls handler once when the server starts the mod, with the
    arguments that follow -- on the modlock-host command line.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_start
        def start(args: list[str]) -> None: ...
    """
    _starts.append(handler)
    return handler


def on_frame[F: Callable[[FrameEvent], object]](handler: F) -> F:
    """on_frame calls handler once per server frame.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_frame
        def frame(event: modlock.FrameEvent) -> None: ...
    """
    _frames.append(handler)
    return handler


class Timer:
    """Timer is one scheduled callback. Time is the game's, so a paused game
    holds its timers. A tick is 1/64 second."""

    def __init__(self) -> None:
        # finished reports whether the timer ran its last time or was
        # cancelled.
        self.finished = False

    def cancel(self) -> None:
        """cancel stops the timer; it does nothing once the timer is finished."""
        self.finished = True


class _Entry(Timer):
    """_Entry is a Timer the clock keeps until it finishes."""

    def __init__(self, delay: float, run: Callable[[], float | None]) -> None:
        super().__init__()
        self.delay = delay
        # run returns the seconds before the entry's next run, or None after
        # its last.
        self.run = run
        # at is the game time the entry next runs. It is unset until started,
        # which waits for the clock when the entry came before the first frame.
        self.started = _now is not None
        self.at = 0.0 if _now is None else _now + delay


# _now is the game time of the latest frame, None before the first.
_now: float | None = None

# _entries are the timers that have not finished, in creation order.
_entries: list[_Entry] = []


def _schedule(delay: float, run: Callable[[], float | None]) -> Timer:
    entry = _Entry(delay, run)
    _entries.append(entry)
    return entry


def after(seconds: float, fn: Callable[[], object]) -> Timer:
    """after runs fn once, seconds from now."""

    def run() -> None:
        fn()

    return _schedule(seconds, run)


def every(seconds: float, fn: Callable[[], object]) -> Timer:
    """every runs fn each seconds, keeping its cadence through a late frame."""

    def run() -> float:
        fn()
        return seconds

    return _schedule(seconds, run)


def sequence(fn: Callable[[int], float | None]) -> Timer:
    """sequence calls fn on the next frame with the number of times it has run,
    starting at 1. fn returns the seconds to wait before it runs again, or None
    to end the sequence."""
    runs = 0

    def run() -> float | None:
        nonlocal runs
        runs += 1
        return fn(runs)

    return _schedule(0, run)


def _advance_timers(seconds: float) -> None:
    """_advance_timers moves the clock to a frame and runs the timers that came
    due, at most once each and in the order they came due. A callback that
    raises ends only its own timer. A clock that runs backward, as a new map's
    does, keeps each timer's remaining delay."""
    global _now, _entries

    # Carry the timers across a clock that went back.
    if _now is not None and seconds < _now:
        shift = seconds - _now
        for entry in _entries:
            if entry.started:
                entry.at += shift
    _now = seconds

    # Start the timers made before the clock was known, then take the due ones.
    # Timers a callback makes wait for the next frame.
    for entry in _entries:
        if not entry.started:
            entry.started = True
            entry.at = seconds + entry.delay
    due = [entry for entry in _entries if not entry.finished and entry.at <= seconds]
    due.sort(key=lambda entry: entry.at)

    for entry in due:
        if entry.finished:
            continue
        try:
            wait = entry.run()
        except Exception as error:
            entry.finished = True
            log(f"timer failed: {error!r}")
            continue
        if wait is None:
            entry.finished = True
        else:
            following = entry.at + wait
            entry.at = following if following > seconds else seconds + wait

    _entries = [entry for entry in _entries if not entry.finished]


def on_world[F: Callable[[str], object]](handler: F) -> F:
    """on_world calls handler with the map's name each time a world loads,
    and at start when one already has. Objects and bots from an earlier
    world are gone by then.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_world
        def world(map: str) -> None: ...
    """
    _worlds.append(handler)
    return handler


def on_input[F: Callable[[Player, Buttons, Buttons], object]](handler: F) -> F:
    """on_input calls handler with the watched buttons a player pressed and
    released before the next frame. A button is held from its press until
    its release.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_input
        def input(player: modlock.Player, pressed: modlock.Buttons, released: modlock.Buttons) -> None: ...
    """
    _inputs.append(handler)
    return handler


def on_restored[F: Callable[[Player, str | None], object]](handler: F) -> F:
    """on_restored calls handler when a player's Player.restore_hero ends:
    the error is None once the target held for a second, or says why it did
    not.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_restored
        def restored(player: modlock.Player, error: str | None) -> None: ...
    """
    _restoreds.append(handler)
    return handler


def on_npcs_restored[F: Callable[[str | None], object]](handler: F) -> F:
    """on_npcs_restored calls handler when restore_npcs ends: the error is
    None once the map holds every target, or says why it does not.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_npcs_restored
        def restored(error: str | None) -> None: ...
    """
    _npcs_restoreds.append(handler)
    return handler


def on_damage[F: Callable[[DamageEvent], DamageResult | None]](handler: F) -> F:
    """on_damage calls handler before each hit lands, so it can block the
    hit or change its damage. Later handlers see the earlier ones'
    amount.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_damage
        def damage(hit: modlock.DamageEvent) -> modlock.DamageResult | None: ...
    """
    _damages.append(handler)
    return handler


def on_chat[F: Callable[[ChatEvent], object]](handler: F) -> F:
    """on_chat calls handler with each chat line a player writes, commands
    included, on the next server frame.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_chat
        def chat(line: modlock.ChatEvent) -> None: ...
    """
    _chats.append(handler)
    return handler


def on_player_join[F: Callable[[PlayerJoinedEvent], object]](handler: F) -> F:
    """on_player_join calls handler once for each player the mod serves when
    they are ready: a human who finished joining, a bot once added, and at
    start each player already in the game.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_player_join
        def join(joined: modlock.PlayerJoinedEvent) -> None: ...
    """
    _joins.append(handler)
    return handler


def on_player_leave[F: Callable[[PlayerLeftEvent], object]](handler: F) -> F:
    """on_player_leave calls handler once for each joined player who leaves
    the mod, with the reason the server dropped them when it gave one. A
    player who disconnects still has their slot and hero during the call. A
    player the mod stops serving leaves too, with disconnected false.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_player_leave
        def leave(left: modlock.PlayerLeftEvent) -> None: ...
    """
    _leaves.append(handler)
    return handler


def on_spawn[F: Callable[[Player, Pawn], object]](handler: F) -> F:
    """on_spawn calls handler each time a player's hero enters play: their
    first hero after joining, each respawn, and each new hero after a change.
    The player has a hero to move, equip and read from then on, which
    on_player_join does not promise. The library reads each player's pawn
    once a frame while a handler is registered.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_spawn
        def spawn(player: modlock.Player, pawn: modlock.Pawn) -> None: ...
    """
    _spawns.append(handler)
    return handler


def _watch_spawns() -> None:
    """_watch_spawns tells the spawn handlers of each hero that entered play
    since the last frame."""
    # A hero is in play while it lives; a placeholder pawn has hero 0.
    present: dict[int, int] = {}
    for connection in players():
        player = connection.player
        pawn = player.pawn()
        if pawn is None or pawn.hero == 0 or pawn.health <= 0:
            continue
        present[player.slot] = pawn.entity

        # A respawn follows a death, and a new hero has a new entity.
        if _spawned.get(player.slot) != pawn.entity:
            for handler in _spawns:
                handler(player, pawn)

    # Forget the fallen and the gone, so their next hero is a spawn.
    _spawned.clear()
    _spawned.update(present)


def on_damaged[F: Callable[[DamagedEvent], object]](handler: F) -> F:
    """on_damaged calls handler on the frame after each hit lands.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_damaged
        def damaged(hit: modlock.DamagedEvent) -> None: ...
    """
    _damageds.append(handler)
    return handler


def on_launch[F: Callable[[LaunchEvent], object]](handler: F) -> F:
    """on_launch calls handler with each watched projectile's first frame,
    before the next frame. watch_projectiles names the projectiles.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_launch
        def launch(projectile: modlock.LaunchEvent) -> None: ...
    """
    _launches.append(handler)
    return handler


def on_impact[F: Callable[[ImpactEvent], object]](handler: F) -> F:
    """on_impact calls handler when a watched projectile strikes something,
    inside the game's impact: its calls apply before the game continues.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_impact
        def impact(hit: modlock.ImpactEvent) -> None: ...
    """
    _impacts.append(handler)
    return handler


def on_landed[F: Callable[[LandedEvent], object]](handler: F) -> F:
    """on_landed calls handler when a hero lands under the manifest's
    QuakeWorld movement, before the next frame.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_landed
        def landed(landing: modlock.LandedEvent) -> None: ...
    """
    _landeds.append(handler)
    return handler


def on_setting_changed[F: Callable[[Player, str, str], object]](handler: F) -> F:
    """on_setting_changed calls handler when a player's setting changes
    outside the mod, such as on the player's profile. The mod's own
    Player.set_setting calls do not reach it.

    Annotate the handler's parameters, since a checker and an editor's
    completion read their types from there:

        @modlock.on_setting_changed
        def changed(player: modlock.Player, key: str, value: str) -> None: ...
    """
    _setting_changes.append(handler)
    return handler


def _start(event: StartEvent) -> StartResult:
    for handler in _starts:
        handler(event.args)
    # Timers run from frames, so the library always takes them.
    return StartResult(
        frames=True,
        damage=bool(_damages),
        damaged=bool(_damageds),
        chat=bool(_chats),
        joins=bool(_joins or _leaves),
    )


# UiValue is an input's new value as the host checked it.
type UiValue = str | float | bool | int | Wheel | None


@dataclasses.dataclass(frozen=True, slots=True)
class _UiHandlers:
    """_UiHandlers are modlock.ui's handlers of the events its interfaces
    raise, which it registers when a mod imports it."""

    # press runs the handler of the button a player pressed.
    press: Callable[[Player, str], object]
    # change runs the handler of the input a player changed.
    change: Callable[[Player, str, UiValue], object]
    # tick advances the interface clock to each frame.
    tick: Callable[[float], object]


# _ui holds modlock.ui's handlers, or None while the mod shows no interface.
_ui: _UiHandlers | None = None


def _serve_ui(handlers: _UiHandlers) -> None:
    """_serve_ui passes presses, input changes and the frame clock to
    modlock.ui, which imports this module and so registers itself."""
    global _ui
    _ui = handlers


def _frame(event: FrameEvent) -> None:
    if _ui is not None:
        _ui.tick(event.time_seconds)
    _advance_timers(event.time_seconds)
    if _spawns:
        _watch_spawns()
    for handler in _frames:
        handler(event)


def _command(event: CommandEvent) -> CommandResult:
    # Split the command line into its name and arguments.
    name, _, args = event.line.strip().partition(" ")
    handler = _commands.get(name)
    if handler is None:
        return CommandResult(claimed=False)

    # Claim the command and run its handler with the next frame. The server
    # reads a client's commands under a CPU budget and drops the client when
    # one handler spends it; slow work in a frame only delays that frame.
    player, text = event.player, args.strip()
    after(0, lambda: handler(player, text))
    return CommandResult(claimed=True)


def _chat(event: ChatEvent) -> None:
    # The line arrives under the same CPU budget as a command, so its
    # handlers run with the next frame.
    def run() -> None:
        for handler in _chats:
            handler(event)

    after(0, run)


def _world(event: WorldEvent) -> None:
    for handler in _worlds:
        handler(event.map)


def _ui_press(event: UiPressEvent) -> None:
    # A press arrives under the same CPU budget as a command, so its handler
    # also runs with the next frame.
    ui = _ui
    if ui is not None:
        player, node = event.player, event.node
        after(0, lambda: ui.press(player, node))


def _ui_input(event: UiInputEvent) -> None:
    # A change runs with the next frame, as a press does.
    ui = _ui
    if ui is not None:
        player, node, value = event.player, event.node, event.value
        after(0, lambda: ui.change(player, node, value))


def _console(method: str, payload: bytes) -> bytes:
    """_console runs one line typed at the mod's console, modlock dev's and
    modlock play's lines that start with >, and returns what Python's own
    console would show: what the line printed, then an expression's value, or
    the error's traceback. The line runs in main.py's module, so it reads and
    changes the mod's own names, and _ holds the last value."""
    scope = sys.modules["main"].__dict__
    shown = io.StringIO()
    printed, sys.stdout = sys.stdout, shown
    try:
        # An expression shows its value; anything else runs as a statement.
        code = payload.decode()
        try:
            expression = compile(code, "<console>", "eval")
        except SyntaxError:
            expression = None
        if expression is None:
            exec(compile(code, "<console>", "exec"), scope)
        else:
            value = eval(expression, scope)
            if value is not None:
                scope["_"] = value
                shown.write(repr(value))
    except Exception as error:
        # Show the error from the line's own frame down.
        frame = error.__traceback__
        shown.write("".join(traceback.format_exception(type(error), error, frame and frame.tb_next)))
    finally:
        sys.stdout = printed
    return shown.getvalue().rstrip().encode()


def _serve(event: ServiceCall) -> ServiceReply:
    handler = _services.get(event.service)
    if handler is None and event.service == "console":
        handler = _console
    if handler is None:
        raise LookupError("the mod serves no service " + event.service)
    return ServiceReply(payload=handler(event.method, event.payload))


def _damage(event: DamageEvent) -> DamageResult | None:
    amount = event.amount
    for handler in _damages:
        change = handler(dataclasses.replace(event, amount=amount))
        if change is not None and change.block:
            return DamageResult(block=True)
        if change is not None and change.amount is not None:
            amount = change.amount
    return None if amount == event.amount else DamageResult(amount=amount)


def _damaged(event: DamagedEvent) -> None:
    for handler in _damageds:
        handler(event)


def _input(event: InputEvent) -> None:
    for handler in _inputs:
        handler(event.player, Buttons(event.pressed), Buttons(event.released))


def _restored(event: RestoredEvent) -> None:
    for handler in _restoreds:
        handler(event.player, event.error or None)


def _npcs_restored(event: NpcsRestoredEvent) -> None:
    for handler in _npcs_restoreds:
        handler(event.error or None)


def _launch(event: LaunchEvent) -> None:
    for handler in _launches:
        handler(event)


def _impact(event: ImpactEvent) -> None:
    for handler in _impacts:
        handler(event)


def _landed(event: LandedEvent) -> None:
    for handler in _landeds:
        handler(event)


def _setting_changed(event: SettingChangedEvent) -> None:
    for handler in _setting_changes:
        handler(event.player, event.key, event.value)


def _players_changed(event: PlayersChangedEvent) -> None:
    global _roster
    _roster = event.players
    for watcher in _player_watchers:
        watcher(_roster)


# A join and a leave run at once, so the leaving player's hero still reads.
def _player_joined(event: PlayerJoinedEvent) -> None:
    for handler in _joins:
        handler(event)


def _player_left(event: PlayerLeftEvent) -> None:
    for handler in _leaves:
        handler(event)


# _HANDLERS delivers each event to the handlers the mod registered.
_HANDLERS: dict[str, Callable[[Any], Any]] = {
    "start": _start,
    "frame": _frame,
    "command": _command,
    "chat": _chat,
    "world": _world,
    "ui_press": _ui_press,
    "ui_input": _ui_input,
    "serve": _serve,
    "damage": _damage,
    "damaged": _damaged,
    "input": _input,
    "restored": _restored,
    "npcs_restored": _npcs_restored,
    "launch": _launch,
    "impact": _impact,
    "landed": _landed,
    "setting_changed": _setting_changed,
    "players_changed": _players_changed,
    "player_joined": _player_joined,
    "player_left": _player_left,
}

_modlock.handle(lambda data: _serve_mod(_HANDLERS, data))
