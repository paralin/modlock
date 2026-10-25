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

# _roster is the player list the host last reported, or None before one.
_roster: list[Connection] | None = None


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
    Registering a name again replaces its handler."""

    def register(handler: F) -> F:
        _commands[name] = handler
        return handler

    return register


def on_start[F: Callable[[list[str]], object]](handler: F) -> F:
    """on_start calls handler once when the server starts the mod, with the
    arguments that follow -- on the modlock-host command line."""
    _starts.append(handler)
    return handler


def on_frame[F: Callable[[FrameEvent], object]](handler: F) -> F:
    """on_frame calls handler once per server frame."""
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
    raises ends only its own timer."""
    global _now, _entries
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
    world are gone by then."""
    _worlds.append(handler)
    return handler


def on_input[F: Callable[[Player, Buttons, Buttons], object]](handler: F) -> F:
    """on_input calls handler with the watched buttons a player pressed and
    released before the next frame. A button is held from its press until
    its release."""
    _inputs.append(handler)
    return handler


def on_restored[F: Callable[[Player, str | None], object]](handler: F) -> F:
    """on_restored calls handler when a player's Player.restore_hero ends:
    the error is None once the target held for a second, or says why it did
    not."""
    _restoreds.append(handler)
    return handler


def on_npcs_restored[F: Callable[[str | None], object]](handler: F) -> F:
    """on_npcs_restored calls handler when restore_npcs ends: the error is
    None once the map holds every target, or says why it does not."""
    _npcs_restoreds.append(handler)
    return handler


def on_damage[F: Callable[[DamageEvent], DamageResult | None]](handler: F) -> F:
    """on_damage calls handler before each hit lands, so it can block the
    hit or change its damage. Later handlers see the earlier ones'
    amount."""
    _damages.append(handler)
    return handler


def on_damaged[F: Callable[[DamagedEvent], object]](handler: F) -> F:
    """on_damaged calls handler on the frame after each hit lands."""
    _damageds.append(handler)
    return handler


def on_launch[F: Callable[[LaunchEvent], object]](handler: F) -> F:
    """on_launch calls handler with each watched projectile's first frame,
    before the next frame. watch_projectiles names the projectiles."""
    _launches.append(handler)
    return handler


def on_impact[F: Callable[[ImpactEvent], object]](handler: F) -> F:
    """on_impact calls handler when a watched projectile strikes something,
    inside the game's impact: its calls apply before the game continues."""
    _impacts.append(handler)
    return handler


def on_landed[F: Callable[[LandedEvent], object]](handler: F) -> F:
    """on_landed calls handler when a hero lands under the manifest's
    QuakeWorld movement, before the next frame."""
    _landeds.append(handler)
    return handler


def on_setting_changed[F: Callable[[Player, str, str], object]](handler: F) -> F:
    """on_setting_changed calls handler when a player's setting changes
    outside the mod, such as on the player's profile. The mod's own
    Player.set_setting calls do not reach it."""
    _setting_changes.append(handler)
    return handler


def _start(event: StartEvent) -> StartResult:
    for handler in _starts:
        handler(event.args)
    # Timers run from frames, so the library always takes them.
    return StartResult(frames=True, damage=bool(_damages), damaged=bool(_damageds))


def _frame(event: FrameEvent) -> None:
    _advance_timers(event.time_seconds)
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


def _world(event: WorldEvent) -> None:
    for handler in _worlds:
        handler(event.map)


def _ui_press(event: UiPressEvent) -> None:
    """_ui_press ignores presses: Python mods build no interface yet."""


def _serve(event: ServiceCall) -> ServiceReply:
    handler = _services.get(event.service)
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


# _HANDLERS delivers each event to the handlers the mod registered.
_HANDLERS: dict[str, Callable[[Any], Any]] = {
    "start": _start,
    "frame": _frame,
    "command": _command,
    "world": _world,
    "ui_press": _ui_press,
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
}

_modlock.handle(lambda data: _serve_mod(_HANDLERS, data))
