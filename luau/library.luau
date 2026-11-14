-- The modlock library writes Modlock mods in Luau. A mod registers its
-- handlers when its main module runs; modlock-host runs the mod in a sandbox
-- inside the game server and calls the handlers on the server's frame
-- thread, one at a time.
--
--   local modlock = require("@modlock")
--
--   modlock.command("hello", function(player)
--   	player:chat("Hello from Luau!")
--   end)
--
-- Build it with modlock build.
--
-- The calls into the game and their types above are generated from the
-- schema. A call that fails logs why in the server log and returns false or
-- nil, so a mod can retry on a later frame instead of stopping. print writes
-- to the server log.

-- Buttons names input bits for blockInput, watchInput and Player.press.
modlock.Buttons = {
	-- attack is the primary fire button.
	attack = 1,
	-- jump is the jump key, Space by default (IN_MANTLE). Blocking it stops the
	-- native jump and mantle; the jumping client still predicts the jump and is
	-- corrected.
	jump = 2 ^ 48,
	-- crouch is the crouch key, Ctrl by default.
	crouch = 2 ^ 2,
	-- dash is the dash key, Shift by default.
	dash = 2 ^ 16,
	-- use is the interact key.
	use = 2 ^ 5,
	-- altFire is the zoom and alternate fire button, right mouse by default.
	altFire = 2 ^ 11,
	-- reload is the reload key.
	reload = 2 ^ 13,
	-- melee is the light melee key, Q by default.
	melee = 2 ^ 32,
	-- ability1 to ability3 are the hero's first three ability keys.
	ability1 = 2 ^ 33,
	ability2 = 2 ^ 34,
	ability3 = 2 ^ 35,
	-- ultimate is the hero's ultimate ability key, the fourth.
	ultimate = 2 ^ 36,
	-- parry is the melee parry button.
	parry = 2 ^ 42,
}

-- Layers names collision layers for trace, as bits.
modlock.Layers = {
	-- solid is the world and solid objects.
	solid = 1,
	-- hitbox is the hit boxes of heroes and units.
	hitbox = 2 ^ 1,
	-- trigger is invisible volumes that react to touch.
	trigger = 2 ^ 2,
	-- playerClip is walls that only stop heroes.
	playerClip = 2 ^ 4,
	-- player is player-controlled heroes.
	player = 2 ^ 18,
	-- npc is units that are not players.
	npc = 2 ^ 19,
	-- staticLevel is the map's built geometry, foliage included.
	staticLevel = 2 ^ 30,
	-- hero, trooper and building are those units' bodies.
	hero = 2 ^ 37,
	trooper = 2 ^ 38,
	building = 2 ^ 40,
}

-- has reports whether bits, a sum of Buttons or Layers bits, holds bit.
-- bit32 reaches only the low 32 bits, and these bits go up to 2^42.
function modlock.has(bits: number, bit: number): boolean
	return bits // bit % 2 == 1
end

-- setOrbitCamera gives the player's hero the hideout's orbit camera, zoomed
-- with the wheel, or returns the normal camera. The hideout HUD replaces the
-- game HUD while it is on. It returns false when the player has no hero or a
-- call fails.
--
-- The pawn's networked m_bInHideoutZone moves its client to the hideout view,
-- and the wheel zooms only while the pawn holds MODIFIER_STATE_HIDEOUT_ZONE,
-- since the server ignores orbit_camera_in and orbit_camera_out without it.
function modlock.setOrbitCamera(player: Player, enabled: boolean): boolean
	local pawn = player:pawn()
	if pawn == nil or pawn.entity == 0 then
		return false
	end
	local flag = modlock.writeField(pawn.entity, "CCitadelPlayerPawn", "m_bInHideoutZone", "bool", enabled)
	local zoom = modlock.holdModifierState(pawn.entity, "MODIFIER_STATE_HIDEOUT_ZONE", enabled)
	return flag and zoom
end

-- Scores report a player's results to the boards a mod declares in mod.json
-- and read those boards back, over the host's scores service. A host without
-- the service, such as a server with no backend, takes no score; the mod
-- logs each one instead, so its author sees what it would report.
--
--   modlock.reportScore(player, "trial.time", elapsedMs, { partition = { course = "canyon" }, key = runId })

-- scoresService names the host service that keeps scores.
local scoresService = "scores"

-- BoardWindow is a span of time a board keeps standings for, in UTC; a week
-- starts Monday.
export type BoardWindow = "allTime" | "day" | "week"

-- MatchOptions describe where a match ranks and the run it came from.
export type MatchOptions = {
	-- partition names a value for each of the board's partitions.
	partition: { [string]: string }?,
	-- key identifies the run, so a resent score counts once.
	key: string?,
	-- earnedAt is when the score was earned, in Unix milliseconds; the host's
	-- clock by default.
	earnedAt: number?,
	-- evidence names what shows the run, such as a recording.
	evidence: string?,
}

-- ScoreOptions describe where a score ranks and the run it came from.
export type ScoreOptions = MatchOptions & {
	-- tiebreak ranks equal values, lower first, such as fewer deaths.
	tiebreak: number?,
}

-- MatchPlace is one player's finishing place in a match, 1 for the winner.
export type MatchPlace = {
	player: Player,
	place: number,
	-- value is the player's score in the match, for a board that keeps one.
	value: number?,
}

-- BoardRead selects a page of a board.
export type BoardRead = {
	-- partition names a value for each of the board's partitions.
	partition: { [string]: string }?,
	-- window is one of the board's windows; all time by default.
	window: BoardWindow?,
	-- offset skips that many rows from the top.
	offset: number?,
	-- limit bounds the rows; ten by default.
	limit: number?,
	-- player asks for this player's own row too.
	player: Player?,
}

-- BoardRow is one player's place on a board.
export type BoardRow = {
	rank: number,
	-- steamId is the player's 64-bit Steam ID in decimal.
	steamId: string,
	name: string,
	value: number,
	tiebreak: number,
	count: number,
}

-- BoardPage is a page of a board.
export type BoardPage = {
	-- rows are the standings in rank order.
	rows: { BoardRow },
	-- player is the asked player's row, when they hold a standing.
	player: BoardRow?,
	-- loading is set until the host has read the board; read again on a later
	-- frame.
	loading: boolean,
}

-- describePartition writes partition values as "[course=canyon]", in name
-- order.
local function describePartition(partition: { [string]: string }?): string
	local names: { string } = {}
	for name, value in partition or {} :: { [string]: string } do
		table.insert(names, `{name}={value}`)
	end
	if #names == 0 then
		return ""
	end
	table.sort(names)
	return `[{table.concat(names, " ")}]`
end

-- taken calls a reporting method and logs line when the host takes no score.
local function taken(method: string, report: string, line: string): boolean
	if modlock.callService(scoresService, method, report) ~= nil then
		return true
	end
	modlock.log(`score {line} (not reported: the host keeps no scores)`)
	return false
end

-- reportScore reports one score for player to a board the manifest declares.
-- It reports whether the host took it; a host without the scores service
-- takes none, and the mod logs the score instead.
function modlock.reportScore(player: Player, board: string, value: number, options: ScoreOptions?): boolean
	local o: ScoreOptions = options or {}
	local report = wire.encode(schema, "ScoreReport", {
		board = board,
		player = player,
		value = math.round(value),
		tiebreak = math.round(o.tiebreak or 0),
		partition = o.partition,
		key = o.key,
		earnedAtUnixMillis = math.round(o.earnedAt or 0),
		evidence = o.evidence,
	})
	return taken("Report", report, `{board}{describePartition(o.partition)}: player {player.slot} {value}`)
end

-- reportMatch reports one match's places to a rating or match board the
-- manifest declares. Players who tie, such as one team's members, share a
-- place. It reports whether the host took it, as reportScore does.
function modlock.reportMatch(board: string, places: { MatchPlace }, options: MatchOptions?): boolean
	-- Encode each place and the line the mod logs when the host takes none.
	local o: MatchOptions = options or {}
	local encoded, lines = {}, {}
	for _, p in places do
		table.insert(encoded, { player = p.player, place = p.place, value = math.round(p.value or 0) })
		table.insert(lines, `player {p.player.slot} #{p.place}`)
	end

	-- Report the match.
	local report = wire.encode(schema, "MatchReport", {
		board = board,
		places = encoded,
		partition = o.partition,
		key = o.key,
		earnedAtUnixMillis = math.round(o.earnedAt or 0),
		evidence = o.evidence,
	})
	return taken("Match", report, `{board}{describePartition(o.partition)}: {table.concat(lines, ", ")}`)
end

-- readBoard reads a page of a board the manifest declares, as the host last
-- read it, or nil when the host serves no scores. A page the host has not
-- read yet comes back loading.
function modlock.readBoard(board: string, read: BoardRead?): BoardPage?
	-- Ask the host for the page; a host without scores answers nothing.
	local r: BoardRead = read or {}
	local query = wire.encode(schema, "BoardQuery", {
		board = board,
		partition = r.partition,
		window = r.window,
		offset = r.offset,
		limit = r.limit,
		player = r.player,
	})
	local reply = modlock.callService(scoresService, "Read", query)
	if reply == nil then
		return nil
	end
	return wire.decode(schema, "BoardPage", reply) :: BoardPage
end

-- Service answers the host's calls to one service the mod serves: the
-- method's name and payload in, the answer out. An error becomes the call's
-- error.
export type Service = (method: string, payload: string) -> string

-- commands maps each registered command name to its handler.
local commands: { [string]: (player: Player, args: string) -> () } = {}

-- services maps each service the mod serves to its handler.
local services: { [string]: Service } = {}

-- ServiceDone ends one callServiceLater: with the reply and no error, or with
-- no reply, the error and the failure that says why.
export type ServiceDone = (reply: string?, error: string?, failure: ServiceFailure) -> ()

-- waiting maps each started service call's number to the function its end
-- runs.
local waiting: { [number]: ServiceDone } = {}

-- starts to settingChanges hold each event's handlers, which run in
-- registration order.
local starts: { (args: { string }) -> () } = {}
local frames: { (frame: FrameEvent) -> () } = {}
local chats: { (line: ChatEvent) -> () } = {}
local joins: { (joined: PlayerJoinedEvent) -> () } = {}
local leaves: { (left: PlayerLeftEvent) -> () } = {}
local worlds: { (map: string) -> () } = {}
local inputs: { (player: Player, pressed: number, released: number) -> () } = {}
local restoreds: { (player: Player, error: string?) -> () } = {}
local npcsRestoreds: { (error: string?) -> () } = {}
local damages: { (hit: DamageEvent) -> DamageResult? } = {}
local damageds: { (hit: DamagedEvent) -> () } = {}
local launches: { (projectile: LaunchEvent) -> () } = {}
local impacts: { (impact: ImpactEvent) -> () } = {}
local landeds: { (landing: LandedEvent) -> () } = {}
local settingChanges: { (player: Player, key: string, value: string) -> () } = {}

-- roster is the player list the host last reported, or nil before one.
local roster: { Connection }? = nil

-- rosterWatchers hear each player list the host reports.
local rosterWatchers: { (players: { Connection }) -> () } = {}

-- Interface takes the events of the players' interfaces: the module that
-- draws them, @modlock/ui, serves them with serveInterface, so the library
-- loads without it.
export type Interface = {
	-- press takes a press on the button with id node, or on the ability bar.
	press: (player: Player, node: string) -> (),
	-- change takes a change to the input with id node, or a wheel step.
	change: (player: Player, node: string, value: (string | number | boolean | Wheel)?) -> (),
	-- tick takes the game clock at each frame, before the timers run.
	tick: (seconds: number) -> (),
	-- players takes each player list the host reports.
	players: (players: { Connection }) -> (),
}

-- InterfaceWire is what serveInterface gives the interface module.
export type InterfaceWire = {
	-- send sends player's interface a change through the host and reports
	-- whether the host took it.
	send: (player: Player, change: any) -> boolean,
	-- encode returns a node's wire encoding, to compare two nodes by.
	encode: (node: any) -> string,
}

-- interface takes the interface events, once a module serves it.
local interface: Interface? = nil

-- serveInterface sends the players' interface events to served from now on
-- and returns the calls the interface module sends its changes through. Mods
-- draw interfaces through @modlock/ui, which calls it once as it loads.
function modlock.serveInterface(served: Interface): InterfaceWire
	interface = served
	table.insert(rosterWatchers, served.players)
	return {
		send = function(player: Player, change: any): boolean
			return call("Ui", "UiRequest", { player = player, change = change }) ~= nil
		end,
		encode = function(node: any): string
			return wire.encode(schema, "Node", node)
		end,
	}
end

-- players lists the occupied player slots in slot order. It answers from the
-- roster the host last reported, and asks the host until a report arrives.
function modlock.players(): { Connection }
	if roster ~= nil then
		return table.clone(roster)
	end
	local response = call("Players", "Empty", {}, "PlayersResponse")
	if response == nil then
		return {}
	end
	return response.players
end

-- connection returns the connection of one player, given the player or their
-- slot: their name, Steam ID, whether they are a bot and whether they
-- finished joining. It returns nil when nobody holds the slot.
function modlock.connection(player: Player | number): Connection?
	local slot = if type(player) == "number" then player else player.slot
	for _, held in roster or modlock.players() do
		if held.player.slot == slot then
			return held
		end
	end
	return nil
end

-- Hero is one released hero.
export type Hero = {
	-- name is the game's name for the hero, such as "hero_atlas".
	name: string,
	-- id is the hero's fixed id, as Pawn.hero reads it.
	id: number,
	-- title is the name players read, such as "Abrams".
	title: string,
	-- portrait is the hero's small stock portrait, as an image's src.
	portrait: string,
}

-- heroes lists every released hero alphabetically by title. The table
-- follows the game's hero data; a hero the game adds or releases joins it in
-- a Modlock release.
local heroes: { Hero } = {
	{ name = "hero_atlas", id = 6, title = "Abrams", portrait = "file://{images}/heroes/bull_sm.psd" },
	{ name = "hero_fencer", id = 77, title = "Apollo", portrait = "file://{images}/heroes/fencer_sm.psd" },
	{ name = "hero_baba", id = 88, title = "Baba", portrait = "file://{images}/heroes/baba_sm.psd" },
	{ name = "hero_bebop", id = 15, title = "Bebop", portrait = "file://{images}/heroes/bebop_sm.psd" },
	{ name = "hero_punkgoat", id = 72, title = "Billy", portrait = "file://{images}/heroes/punkgoat_sm.psd" },
	{ name = "hero_nano", id = 16, title = "Calico", portrait = "file://{images}/heroes/nano_sm.psd" },
	{ name = "hero_unicorn", id = 81, title = "Celeste", portrait = "file://{images}/heroes/unicorn_sm.psd" },
	{ name = "hero_deadpack", id = 78, title = "Deadman Danny", portrait = "file://{images}/heroes/deadpack_sm.psd" },
	{ name = "hero_drifter", id = 64, title = "Drifter", portrait = "file://{images}/heroes/drifter_sm.psd" },
	{ name = "hero_dynamo", id = 11, title = "Dynamo", portrait = "file://{images}/heroes/sumo_sm.psd" },
	{ name = "hero_necro", id = 76, title = "Graves", portrait = "file://{images}/heroes/necro_sm.psd" },
	{ name = "hero_orion", id = 17, title = "Grey Talon", portrait = "file://{images}/heroes/archer_sm.psd" },
	{ name = "hero_haze", id = 13, title = "Haze", portrait = "file://{images}/heroes/haze_sm.psd" },
	{ name = "hero_astro", id = 14, title = "Holliday", portrait = "file://{images}/heroes/astro_sm.psd" },
	{ name = "hero_inferno", id = 1, title = "Infernus", portrait = "file://{images}/heroes/inferno_sm.psd" },
	{ name = "hero_tengu", id = 20, title = "Ivy", portrait = "file://{images}/heroes/tengu_sm.psd" },
	{ name = "hero_kelvin", id = 12, title = "Kelvin", portrait = "file://{images}/heroes/kelvin_sm.psd" },
	{ name = "hero_ghost", id = 4, title = "Lady Geist", portrait = "file://{images}/heroes/spectre_sm.psd" },
	{ name = "hero_lash", id = 31, title = "Lash", portrait = "file://{images}/heroes/lash_sm.psd" },
	{ name = "hero_forge", id = 8, title = "McGinnis", portrait = "file://{images}/heroes/engineer_sm.psd" },
	{ name = "hero_vampirebat", id = 63, title = "Mina", portrait = "file://{images}/heroes/vampirebat_sm.psd" },
	{ name = "hero_mirage", id = 52, title = "Mirage", portrait = "file://{images}/heroes/mirage_sm.psd" },
	{ name = "hero_krill", id = 18, title = "Mo & Krill", portrait = "file://{images}/heroes/digger_sm.psd" },
	{ name = "hero_nurse", id = 87, title = "Nurse Harrow", portrait = "file://{images}/heroes/nurse_sm.psd" },
	{ name = "hero_bookworm", id = 67, title = "Paige", portrait = "file://{images}/heroes/bookworm_sm.psd" },
	{ name = "hero_chrono", id = 10, title = "Paradox", portrait = "file://{images}/heroes/chrono_sm.psd" },
	{ name = "hero_synth", id = 50, title = "Pocket", portrait = "file://{images}/heroes/synth_sm.psd" },
	{ name = "hero_ratking", id = 84, title = "Rat King", portrait = "file://{images}/heroes/ratking_sm.psd" },
	{ name = "hero_familiar", id = 79, title = "Rem", portrait = "file://{images}/heroes/familiar_sm.psd" },
	{ name = "hero_gigawatt", id = 2, title = "Seven", portrait = "file://{images}/heroes/gigawatt_sm.psd" },
	{ name = "hero_shiv", id = 19, title = "Shiv", portrait = "file://{images}/heroes/shiv_sm.psd" },
	{ name = "hero_werewolf", id = 80, title = "Silver", portrait = "file://{images}/heroes/werewolf_sm.psd" },
	{ name = "hero_magician", id = 60, title = "Sinclair", portrait = "file://{images}/heroes/magician_sm.psd" },
	{ name = "hero_chessmaster", id = 85, title = "Solomon", portrait = "file://{images}/heroes/chessmaster_sm.psd" },
	{ name = "hero_doorman", id = 69, title = "The Doorman", portrait = "file://{images}/heroes/doorman_sm.psd" },
	{ name = "hero_priest", id = 65, title = "Venator", portrait = "file://{images}/heroes/priest_sm.psd" },
	{ name = "hero_frank", id = 66, title = "Victor", portrait = "file://{images}/heroes/frank_sm.psd" },
	{ name = "hero_hornet", id = 3, title = "Vindicta", portrait = "file://{images}/heroes/hornet_sm.png" },
	{ name = "hero_artist", id = 86, title = "Violet", portrait = "file://{images}/heroes/artist_sm.psd" },
	{ name = "hero_viscous", id = 35, title = "Viscous", portrait = "file://{images}/heroes/viscous_sm.psd" },
	{ name = "hero_viper", id = 58, title = "Vyper", portrait = "file://{images}/heroes/kali_sm.psd" },
	{ name = "hero_warden", id = 25, title = "Warden", portrait = "file://{images}/heroes/warden_sm.psd" },
	{ name = "hero_wraith", id = 7, title = "Wraith", portrait = "file://{images}/heroes/wraith_sm.psd" },
	{ name = "hero_yamato", id = 27, title = "Yamato", portrait = "file://{images}/heroes/yamato_sm.psd" },
}
modlock.heroes = heroes

-- plainTitle is a hero's title in lower case without spaces or punctuation.
local function plainTitle(hero: Hero): string
	return (string.gsub(string.lower(hero.title), "[^a-z0-9]", ""))
end

-- heroesByName holds each released hero under every name heroNamed accepts,
-- reduced as heroNamed reduces them. Game names go in last, so a title that
-- equals another hero's game name could never take it.
local heroesByName: { [string]: Hero } = {}
for _, hero in heroes do
	heroesByName[plainTitle(hero)] = hero
end
for _, hero in heroes do
	heroesByName[string.sub(hero.name, #"hero_" + 1)] = hero
	heroesByName[hero.name] = hero
end

-- heroNamed returns the released hero that name names exactly: its game name,
-- such as "hero_nano", that name without "hero_", such as "nano", or its
-- title in any case, with or without spaces and punctuation, such as "Calico"
-- or "ladygeist". Unlike findHero it matches no part of a name, so a
-- misspelled name finds no hero rather than another one.
function modlock.heroNamed(name: string): Hero?
	return heroesByName[(string.gsub(string.lower(name), "[^a-z0-9_]", ""))]
end
heroNamed = modlock.heroNamed

-- findHero matches what a player typed, such as "geist", "Lady Geist" or
-- "hero_ghost", to a hero: an exact game name or title first, then the first
-- title that starts with or contains it.
function modlock.findHero(typed: string): Hero?
	-- Reduce what was typed to letters, digits and underscores.
	local key = string.gsub(string.lower(typed), "[^a-z0-9_]", "")
	if key == "" then
		return nil
	end

	-- Compare it with each name and plain title.
	for _, hero in heroes do
		if hero.name == key or plainTitle(hero) == key then
			return hero
		end
	end
	for _, hero in heroes do
		if string.sub(plainTitle(hero), 1, #key) == key then
			return hero
		end
	end
	for _, hero in heroes do
		if string.find(plainTitle(hero), key, 1, true) then
			return hero
		end
	end
	return nil
end

-- Want is what a member's hero should be; each field it leaves out is free.
export type Want = {
	-- hero seats the member on a hero: its name as players know it or as the
	-- game does, such as "Calico" or "hero_nano", its id, or "any" for the hero
	-- they have or else a random one.
	hero: (string | number)?,
	-- team is the team, 2 or 3, to seat the member on. Without one a member
	-- keeps their hero's team, or else joins the side with fewer heroes.
	team: number?,
	-- respawn is the seconds a fallen hero waits before Cast asks for it back.
	respawn: number?,
}

-- CastOptions are a mode's answers to its players coming and going.
export type CastOptions<M> = {
	-- join makes the member for a human who finished joining.
	join: (connection: Connection) -> M,
	-- leave ends a member whose connection left the slot.
	leave: ((member: M, connection: Connection) -> ())?,
	-- want is what every member wants, or a function that returns what one
	-- member wants now. Cast calls it once per member each frame, so it should
	-- return a want it keeps rather than make a new one.
	want: (Want | (member: M) -> Want)?,
	-- bots says what to do with a bot: skip leaves it out of the cast, join
	-- makes it a member as a human is, and kick asks the server to remove it,
	-- once per connection. Skip is the default.
	bots: ("skip" | "join" | "kick")?,
}

-- Cast keeps a mode's players: one member for each human who finished
-- joining, made by the mode's join and ended by its leave. Members are keyed
-- by slot and connection generation, so a reconnect is a leave and a join.
-- Cast learns of joins and leaves from the host's player reports and settles
-- them on the next frame; it never walks the player list on a frame where
-- nothing changed.
--
-- A member may carry a want: to sit on a hero and team, and to come back a
-- while after its hero falls. The mode gives one want for every member or a
-- function that reads it from the member each frame, so a want follows the
-- member's own state. A human whose want leaves the hero free still gets one
-- when the game leaves them on a team without a hero, as its own spawn on
-- joining can, since nothing else would ever seat them. Each frame Cast
-- compares the want with what the member's hero shows, asks the game again at
-- most once a second while they differ, and stops asking once they match.
-- That is a reconciliation loop: a lost or refused ask is only asked again,
-- never reported as a failure.
--
-- A frame costs one pawn read per member, which the mode reads back with
-- pawn instead of asking the game again.
export type Cast<M> = {
	-- size returns the number of members.
	size: (self: Cast<M>) -> number,
	-- members lists the members in the order they joined.
	members: (self: Cast<M>) -> { M },
	-- get returns the member in player's slot, if any.
	get: (self: Cast<M>, player: Player) -> M?,
	-- pawn returns the member's hero as Cast read it this frame.
	pawn: (self: Cast<M>, player: Player) -> Pawn?,
	-- frame settles the joins and leaves reported since the last frame, reads
	-- each member's hero, and asks the game once a second for any want it does
	-- not show yet. seconds is the game clock.
	frame: (self: Cast<M>, seconds: number) -> (),
}

-- Seat is one member with what Cast last saw of their hero.
type Seat<M> = {
	-- connection is the member's connection, whose generation keys the seat.
	connection: Connection,
	-- member is what the mode's join made.
	member: M,
	-- pawn is the member's hero this frame.
	pawn: Pawn?,
	-- alive is whether the member's hero stood with health this frame.
	alive: boolean,
	-- fellAt is when Cast last saw the hero fall.
	fellAt: number,
	-- hero is the member's hero last frame, or 0 without one.
	hero: number,
	-- heroLostAt is when the member last went from a hero to none.
	heroLostAt: number,
	-- joinedAt is when the member joined.
	joinedAt: number,
	-- askedAt is when Cast last asked the game for the want.
	askedAt: number,
}

-- CastState is a Cast with the state its methods keep.
type CastState<M> = Cast<M> & {
	-- options are the mode's answers.
	options: CastOptions<M>,
	-- seats maps each member's slot to their seat.
	seats: { [number]: Seat<M> },
	-- order holds the seats in the order their members joined.
	order: { Seat<M> },
	-- kicked holds the connection generation each kicked bot's slot had.
	kicked: { [number]: number },
	-- reported is the player list the host reported since the last frame.
	reported: { Connection }?,
}

-- changeSeconds is how long Cast leaves a member without a hero after they
-- had one, or after they joined when their hero is free. The game's own
-- Change Hero removes the hero before it spawns the new one, and seating the
-- member in that gap would override their pick.
local changeSeconds = 3

-- noWant leaves a member's hero free.
local noWant: Want = {}

local Cast = {}
Cast.__index = Cast
modlock.Cast = Cast

-- new returns a Cast that keeps one member per human player from now on,
-- starting with the players the host last reported.
function Cast.new<M>(options: CastOptions<M>): Cast<M>
	local cast = {
		options = options,
		seats = {},
		order = {},
		kicked = {},
		reported = roster,
	}
	table.insert(rosterWatchers, function(players: { Connection })
		cast.reported = players
	end)
	return setmetatable(cast, Cast) :: any
end

-- size returns the number of members.
function Cast.size<M>(self: CastState<M>): number
	return #self.order
end

-- members lists the members in the order they joined.
function Cast.members<M>(self: CastState<M>): { M }
	local members: { M } = {}
	for _, seat in self.order do
		table.insert(members, seat.member)
	end
	return members
end

-- get returns the member in player's slot, if any.
function Cast.get<M>(self: CastState<M>, player: Player): M?
	local seat = self.seats[player.slot]
	return if seat then seat.member else nil
end

-- pawn returns the member's hero as Cast read it this frame.
function Cast.pawn<M>(self: CastState<M>, player: Player): Pawn?
	local seat = self.seats[player.slot]
	return if seat then seat.pawn else nil
end

-- kick asks the server to remove a bot, once per connection.
local function kick<M>(cast: CastState<M>, connection: Connection)
	local slot = connection.player.slot
	if cast.kicked[slot] == connection.generation then
		return
	end
	cast.kicked[slot] = connection.generation
	modlock.serverCommand(`kick "{connection.name}"`)
end

-- settle ends the members whose connection left and makes the new ones,
-- joined at now.
local function settle<M>(cast: CastState<M>, players: { Connection }, now: number)
	-- A member leaves when their slot empties or a new connection takes it.
	local present: { [number]: number } = {}
	for _, connection in players do
		present[connection.player.slot] = connection.generation
	end
	for _, seat in table.clone(cast.order) do
		local slot = seat.connection.player.slot
		if present[slot] == seat.connection.generation then
			continue
		end
		cast.seats[slot] = nil
		table.remove(cast.order, table.find(cast.order, seat))
		local leave = cast.options.leave
		if leave then
			leave(seat.member, seat.connection)
		end
	end

	-- A human who finished joining becomes a member, and a bot as the
	-- policy says.
	local bots = cast.options.bots or "skip"
	for _, connection in players do
		local slot = connection.player.slot
		if not connection.ready or cast.seats[slot] ~= nil then
			continue
		end
		if connection.bot and bots ~= "join" then
			if bots == "kick" then
				kick(cast, connection)
			end
			continue
		end
		local seat: Seat<M> = {
			connection = connection,
			member = cast.options.join(connection),
			pawn = nil,
			alive = false,
			fellAt = -math.huge,
			hero = 0,
			heroLostAt = -math.huge,
			joinedAt = now,
			askedAt = -math.huge,
		}
		cast.seats[slot] = seat
		table.insert(cast.order, seat)
	end
end

-- stranded returns "any" for a human whose hero is free when the game has
-- left them on a team without one for changeSeconds since they joined, and
-- otherwise leaves the hero free. Such a pawn never gets a hero on its own,
-- while a player still choosing one has no pawn and a spectator stands on
-- team 1. A bot is left to the game, which seats it.
local function stranded<M>(seat: Seat<M>, now: number): string?
	local pawn = seat.pawn
	local heroless = pawn ~= nil and pawn.hero == 0 and (pawn.team == 2 or pawn.team == 3)
	return if heroless and not seat.connection.bot and now - seat.joinedAt >= changeSeconds
		then "any"
		else nil
end

-- wantOf is what the member in seat wants now.
local function wantOf<M>(cast: CastState<M>, seat: Seat<M>): Want
	local want = cast.options.want
	if want == nil then
		return noWant
	end
	if type(want) == "function" then
		return want(seat.member)
	end
	return want
end

-- seated reports whether the member's hero shows the hero and team they want.
local function seated<M>(seat: Seat<M>, want: Want, hero: string | number): boolean
	-- A member needs a hero on the team they want before the hero counts.
	local pawn = seat.pawn
	if pawn == nil or pawn.hero == 0 then
		return false
	end
	if want.team ~= nil and pawn.team ~= want.team then
		return false
	end
	if hero == "any" then
		return true
	end

	-- A name outside the released heroes, such as one the game added since,
	-- is met by whatever hero the game gave for it.
	local known = if type(hero) == "string" then modlock.heroNamed(hero) else nil
	local id = if type(hero) == "number" then hero elseif known then known.id else nil
	return id == nil or pawn.hero == id
end

-- side is the team a member joins: their hero's, or the one with fewer heroes.
local function side<M>(cast: CastState<M>, seat: Seat<M>): number
	-- A hero keeps its team. The game puts each connecting player on a team
	-- before they have a hero, so a member without one is counted afresh.
	local pawn = seat.pawn
	if pawn ~= nil and pawn.hero ~= 0 and (pawn.team == 2 or pawn.team == 3) then
		return pawn.team
	end

	-- Count the other members' heroes on each team.
	local amber, sapphire = 0, 0
	for _, other in cast.order do
		local theirs = other.pawn
		if other == seat or theirs == nil or theirs.hero == 0 then
			continue
		end
		if theirs.team == 2 then
			amber += 1
		elseif theirs.team == 3 then
			sapphire += 1
		end
	end
	return if sapphire < amber then 3 else 2
end

-- seatHero asks the game to put the member on hero and their team.
local function seatHero<M>(cast: CastState<M>, seat: Seat<M>, want: Want, hero: string | number)
	-- Any hero keeps the one the member has, or else takes a random one.
	local pawn = seat.pawn
	local pick: string | number = hero
	if hero == "any" then
		pick = if pawn ~= nil and pawn.hero ~= 0 then pawn.hero else heroes[math.random(#heroes)].name
	end
	seat.connection.player:selectHero(pick, if want.team ~= nil then want.team else side(cast, seat))
end

-- reconcile reads a member's hero and asks for the part of the want it lacks.
local function reconcile<M>(cast: CastState<M>, seat: Seat<M>, now: number)
	-- Note when the hero falls.
	local pawn = seat.connection.player:pawn()
	local alive = pawn ~= nil and pawn.hero ~= 0 and pawn.health > 0
	if seat.alive and not alive then
		seat.fellAt = now
	end
	seat.pawn = pawn
	seat.alive = alive

	-- Note when a member who had a hero is left without one, so a hero change
	-- the game is making finishes before Cast seats them again.
	local had = if pawn ~= nil then pawn.hero else 0
	if had == 0 and seat.hero ~= 0 then
		seat.heroLostAt = now
	end
	seat.hero = had
	local changing = had == 0 and now - seat.heroLostAt < changeSeconds

	-- Ask at most once a second, first for the seat, then for the hero back.
	if now - seat.askedAt < 1 or changing then
		return
	end
	local want = wantOf(cast, seat)
	local respawn = want.respawn
	local hero: (string | number)? = want.hero
	if hero == nil then
		hero = stranded(seat, now)
	end
	if hero ~= nil and not seated(seat, want, hero) then
		seat.askedAt = now
		seatHero(cast, seat, want, hero)
	elseif respawn ~= nil and pawn ~= nil and pawn.hero ~= 0 and not alive and now - seat.fellAt >= respawn then
		seat.askedAt = now
		seat.connection.player:respawn()
	end
end

-- frame settles the joins and leaves reported since the last frame, reads
-- each member's hero, and asks the game once a second for any want it does
-- not show yet. seconds is the game clock.
function Cast.frame<M>(self: CastState<M>, seconds: number)
	-- Settle the player list the host reported since the last frame.
	local reported = self.reported
	if reported ~= nil then
		settle(self, reported, seconds)
		self.reported = nil
	end

	-- Reconcile each member's hero with their want.
	for _, seat in self.order do
		reconcile(self, seat, seconds)
	end
end

-- serve answers the host's calls to service with handler, replacing its
-- earlier handler.
function modlock.serve(service: string, handler: Service)
	services[service] = handler
end

-- callServiceLater calls method of a service the host provides and returns
-- at once; done runs in a later frame with the answer, or with an error when
-- the service refused, the call waited past timeout seconds or nobody
-- provides the service. Zero or nil waits the host's longest, 30 seconds.
-- done runs once while the mod runs; calls still waiting when the mod stops
-- end with it.
function modlock.callServiceLater(service: string, method: string, payload: string, done: ServiceDone, timeout: number?)
	-- A call the host refuses to start ends in the next frame as well, so
	-- done never runs before this returns.
	local call = modlock.startServiceCall(service, method, payload, math.floor((timeout or 0) * 1000))
	if call == nil then
		modlock.after(0, function()
			done(nil, "the host did not start the call to " .. service, "unavailable")
		end)
		return
	end
	waiting[call] = done
end

-- command calls handler when a player types /name in chat or their client
-- sends the console command name to the server. The handler runs with the
-- next server frame, before the frame handlers, and receives the text after
-- the name, trimmed of surrounding spaces. Registering a name again replaces
-- its handler.
function modlock.command(name: string, handler: (player: Player, args: string) -> ())
	commands[name] = handler
end

-- onStart calls handler once when the server starts the mod, with the
-- arguments that follow -- on the modlock-host command line.
function modlock.onStart(handler: (args: { string }) -> ())
	table.insert(starts, handler)
end

-- onFrame calls handler once per server frame.
function modlock.onFrame(handler: (frame: FrameEvent) -> ())
	table.insert(frames, handler)
end

-- Timer is one scheduled callback.
export type Timer = {
	-- finished reports whether the timer ran its last time or was cancelled.
	finished: boolean,
	-- cancel stops the timer; it does nothing once the timer is finished.
	cancel: (self: Timer) -> (),
}

-- Entry is a Timer the clock keeps until it finishes.
type Entry = Timer & {
	id: number,
	-- at is the game time the entry next runs, unset until the clock is known.
	at: number?,
	delay: number,
	-- run returns the seconds before the entry's next run, or nil after its
	-- last.
	run: () -> number?,
}

local timerMethods = {}
timerMethods.__index = timerMethods

function timerMethods.cancel(self: Entry)
	self.finished = true
end

-- now is the game time of the latest frame, unset before the first.
local now: number? = nil
local entries: { Entry } = {}
local lastTimer = 0

-- schedule adds a timer whose first run is delay seconds away.
local function schedule(delay: number, run: () -> number?): Timer
	lastTimer += 1
	local entry = setmetatable({
		finished = false,
		id = lastTimer,
		at = if now then now + delay else nil,
		delay = delay,
		run = run,
	}, timerMethods) :: any
	table.insert(entries, entry)
	return entry
end

-- after runs fn once, seconds from now. Time is the game's, so a paused game
-- holds its timers. A tick is 1/64 second.
function modlock.after(seconds: number, fn: () -> ()): Timer
	return schedule(seconds, function()
		fn()
		return nil
	end)
end

-- every runs fn each seconds, keeping its cadence through a late frame.
function modlock.every(seconds: number, fn: () -> ()): Timer
	return schedule(seconds, function()
		fn()
		return seconds
	end)
end

-- sequence calls fn on the next frame with the number of times it has run,
-- starting at 1. fn returns the seconds to wait before it runs again, or
-- nothing to end the sequence.
function modlock.sequence(fn: (run: number) -> number?): Timer
	local runs = 0
	return schedule(0, function()
		runs += 1
		return fn(runs)
	end)
end

-- advanceTimers moves the clock to a frame and runs the timers that came due,
-- at most once each and in the order they came due. A callback that errors ends
-- only its own timer. A clock that runs backward, as a new map's does, keeps
-- each timer's remaining delay.
local function advanceTimers(seconds: number)
	-- Carry the timers across a clock that went back.
	if now and seconds < now then
		local shift = seconds - now
		for _, entry in entries do
			if entry.at then
				entry.at = (entry.at :: number) + shift
			end
		end
	end
	now = seconds

	-- Start the timers made before the clock was known, then take the due ones.
	-- Timers a callback makes wait for the next frame.
	local due: { Entry } = {}
	for _, entry in entries do
		if entry.at == nil then
			entry.at = seconds + entry.delay
		end
		if not entry.finished and (entry.at :: number) <= seconds then
			table.insert(due, entry)
		end
	end
	table.sort(due, function(a: Entry, b: Entry)
		if a.at ~= b.at then
			return (a.at :: number) < (b.at :: number)
		end
		return a.id < b.id
	end)

	for _, entry in due do
		if entry.finished then
			continue
		end
		local ok, wait = pcall(entry.run)
		if not ok then
			entry.finished = true
			modlock.log("timer failed: " .. tostring(wait))
		elseif wait == nil then
			entry.finished = true
		else
			local delay = wait :: number
			local next = (entry.at :: number) + delay
			entry.at = if next > seconds then next else seconds + delay
		end
	end

	local live: { Entry } = {}
	for _, entry in entries do
		if not entry.finished then
			table.insert(live, entry)
		end
	end
	entries = live
end

-- onWorld calls handler with the map's name each time a world loads, and at
-- start when one already has. Objects and bots from an earlier world are
-- gone by then.
function modlock.onWorld(handler: (map: string) -> ())
	table.insert(worlds, handler)
end

-- onInput calls handler with the watched buttons a player pressed and
-- released, as Buttons bits, before the next frame. A button is held from
-- its press until its release.
function modlock.onInput(handler: (player: Player, pressed: number, released: number) -> ())
	table.insert(inputs, handler)
end

-- onRestored calls handler when a player's Player.restoreHero ends: error is
-- nil once the target held for a second, or says why it did not.
function modlock.onRestored(handler: (player: Player, error: string?) -> ())
	table.insert(restoreds, handler)
end

-- onNpcsRestored calls handler when restoreNpcs ends: error is nil once the
-- map holds every target, or says why it does not.
function modlock.onNpcsRestored(handler: (error: string?) -> ())
	table.insert(npcsRestoreds, handler)
end

-- onDamage calls handler before each hit lands, so it can block the hit or
-- change its damage. Later handlers see the earlier ones' amount.
function modlock.onDamage(handler: (hit: DamageEvent) -> DamageResult?)
	table.insert(damages, handler)
end

-- onChat calls handler with each chat line a player writes, commands
-- included, on the next server frame.
function modlock.onChat(handler: (line: ChatEvent) -> ())
	table.insert(chats, handler)
end

-- onPlayerJoin calls handler once for each player the mod serves when they
-- are ready: a human who finished joining, a bot once added, and at start
-- each player already in the game.
function modlock.onPlayerJoin(handler: (joined: PlayerJoinedEvent) -> ())
	table.insert(joins, handler)
end

-- onPlayerLeave calls handler once for each joined player who leaves the mod,
-- with the reason the server dropped them when it gave one. A player who
-- disconnects still has their slot and hero during the call. A player the
-- mod stops serving leaves too, with disconnected false.
function modlock.onPlayerLeave(handler: (left: PlayerLeftEvent) -> ())
	table.insert(leaves, handler)
end

-- Seating is where seatPlayers puts each player who joins.
export type Seating = {
	-- hero is each player's hero: its name as players know it or as the game
	-- does, such as "Calico" or "hero_nano", its id, or "any" for the hero they
	-- have or else a random one. "any" is the default.
	hero: (string | number)?,
	-- team is the team, 2 or 3, every player joins, or a function that picks
	-- the team for one player as they join. Without one each player joins the
	-- side with fewer heroes.
	team: (number | (connection: Connection) -> number)?,
	-- respawn is the seconds a fallen hero waits before it comes back. Without
	-- one a fallen hero is left to the game.
	respawn: number?,
}

-- seatPlayers gives every player who joins a hero on a team, skipping the
-- game's team and hero screens, and keeps them seated for the rest of the
-- game: it brings a fallen hero back after the respawn delay and seats a
-- player again after a hero change. It returns the Cast that does the
-- seating, whose members are the players' connections, and runs it each frame
-- itself.
function modlock.seatPlayers(seating: Seating?): Cast<Connection>
	-- Decide each player's seat once, as they join.
	local options: Seating = seating or {}
	local hero: string | number = options.hero or "any"
	local team = options.team
	local wants: { [Connection]: Want } = {}
	local cast: Cast<Connection> = Cast.new({
		join = function(connection: Connection): Connection
			wants[connection] = {
				hero = hero,
				team = if type(team) == "function" then team(connection) else team,
				respawn = options.respawn,
			}
			return connection
		end,
		leave = function(member: Connection)
			wants[member] = nil
		end,
		want = function(member: Connection): Want
			return wants[member]
		end,
	})

	-- Run the cast with the mod's frames.
	table.insert(frames, function(frame: FrameEvent)
		cast:frame(frame.timeSeconds)
	end)
	return cast
end

-- spawns holds the spawn handlers, which run in registration order.
local spawns: { (player: Player, pawn: Pawn) -> () } = {}

-- spawned maps each slot whose hero is in play to that hero's entity, as the
-- spawn handlers last heard of it.
local spawned: { [number]: number } = {}

-- onSpawn calls handler each time a player's hero enters play: their first
-- hero after joining, each respawn, and each new hero after a change. The
-- player has a hero to move, equip and read from then on, which onPlayerJoin
-- does not promise. The library reads each player's pawn once a frame while a
-- handler is registered.
function modlock.onSpawn(handler: (player: Player, pawn: Pawn) -> ())
	table.insert(spawns, handler)
end

-- watchSpawns tells the spawn handlers of each hero that entered play since
-- the last frame.
local function watchSpawns()
	-- A hero is in play while it lives; a placeholder pawn has hero 0.
	local present: { [number]: number } = {}
	for _, connection in modlock.players() do
		local player = connection.player
		local pawn = player:pawn()
		if pawn and pawn.hero ~= 0 and pawn.health > 0 then
			present[player.slot] = pawn.entity

			-- A respawn follows a death, and a new hero has a new entity.
			if spawned[player.slot] ~= pawn.entity then
				for _, handler in spawns do
					handler(player, pawn)
				end
			end
		end
	end

	-- Forget the fallen and the gone, so their next hero is a spawn.
	spawned = present
end

-- onDamaged calls handler on the frame after each hit lands.
function modlock.onDamaged(handler: (hit: DamagedEvent) -> ())
	table.insert(damageds, handler)
end

-- onLaunch calls handler with each watched projectile's first frame, before
-- the next frame. watchProjectiles names the projectiles.
function modlock.onLaunch(handler: (projectile: LaunchEvent) -> ())
	table.insert(launches, handler)
end

-- onImpact calls handler when a watched projectile strikes something, inside
-- the game's impact: its calls apply before the game continues.
function modlock.onImpact(handler: (impact: ImpactEvent) -> ())
	table.insert(impacts, handler)
end

-- onLanded calls handler when a hero lands under the manifest's QuakeWorld
-- movement, before the next frame.
function modlock.onLanded(handler: (landing: LandedEvent) -> ())
	table.insert(landeds, handler)
end

-- onSettingChanged calls handler when a player's setting changes outside the
-- mod, such as on the player's profile. The mod's own player:setSetting calls
-- do not reach it.
function modlock.onSettingChanged(handler: (player: Player, key: string, value: string) -> ())
	table.insert(settingChanges, handler)
end

-- handlers delivers each event to the handlers the mod registered.
local handlers: ModHandlers = {
	start = function(event)
		for _, handler in starts do
			handler(event.args)
		end
		-- Timers run from frames, so the library always takes them.
		return {
			frames = true,
			damage = #damages ~= 0,
			damaged = #damageds ~= 0,
			chat = #chats ~= 0,
			joins = #joins ~= 0 or #leaves ~= 0,
		}
	end,

	frame = function(event)
		if interface then
			interface.tick(event.timeSeconds)
		end
		advanceTimers(event.timeSeconds)
		if #spawns ~= 0 then
			watchSpawns()
		end
		for _, handler in frames do
			handler(event)
		end
	end,

	command = function(event)
		-- Split the command line into its name and arguments.
		local name, args = string.match(event.line, "^%s*(%S+)%s*(.-)%s*$")
		local handler = if name then commands[name] else nil
		if handler == nil then
			return { claimed = false }
		end

		-- Claim the command and run its handler with the next frame. The server
		-- reads a client's commands under a CPU budget and drops the client when
		-- one handler spends it; slow work in a frame only delays that frame.
		local player = event.player
		modlock.after(0, function()
			handler(player, args or "")
		end)
		return { claimed = true }
	end,

	-- A line arrives under the same CPU budget as a command, so its handlers
	-- run with the next frame.
	chat = function(event)
		modlock.after(0, function()
			for _, handler in chats do
				handler(event)
			end
		end)
	end,

	world = function(event)
		for _, handler in worlds do
			handler(event.map)
		end
	end,

	-- A press or a change arrives under the same CPU budget as a command, so
	-- the interface's handlers run with the next frame.
	uiPress = function(event)
		local served = interface
		if served then
			modlock.after(0, function()
				served.press(event.player, event.node)
			end)
		end
	end,
	uiInput = function(event)
		local served = interface
		if served then
			modlock.after(0, function()
				served.change(event.player, event.node, event.value)
			end)
		end
	end,

	serve = function(event)
		local handler = services[event.service]
		if handler == nil then
			error(`the mod serves no service {event.service}`, 0)
		end
		return { payload = handler(event.method, event.payload) }
	end,

	damage = function(event)
		-- Offer the hit to each handler in turn, with the amount the earlier
		-- ones set; a block ends the offer.
		local amount = event.amount
		for _, handler in damages do
			local hit = table.clone(event)
			hit.amount = amount
			local change = handler(hit)
			if change and change.block then
				return { block = true }
			end
			amount = if change and change.amount then change.amount else amount
		end

		-- Answer nothing when no handler changed the hit.
		return if amount == event.amount then {} else { amount = amount }
	end,

	damaged = function(event)
		for _, handler in damageds do
			handler(event)
		end
	end,

	input = function(event)
		for _, handler in inputs do
			handler(event.player, event.pressed, event.released)
		end
	end,

	restored = function(event)
		for _, handler in restoreds do
			handler(event.player, if event.error ~= "" then event.error else nil)
		end
	end,

	serviceReplied = function(event)
		local done = waiting[event.call]
		if done == nil then
			return
		end
		waiting[event.call] = nil
		if event.failure ~= nil and event.failure ~= "none" then
			done(nil, event.error, event.failure)
			return
		end
		done(event.payload, nil, "none")
	end,

	npcsRestored = function(event)
		for _, handler in npcsRestoreds do
			handler(if event.error ~= "" then event.error else nil)
		end
	end,

	launch = function(event)
		for _, handler in launches do
			handler(event)
		end
	end,

	impact = function(event)
		for _, handler in impacts do
			handler(event)
		end
	end,

	landed = function(event)
		for _, handler in landeds do
			handler(event)
		end
	end,

	settingChanged = function(event)
		for _, handler in settingChanges do
			handler(event.player, event.key, event.value)
		end
	end,

	playersChanged = function(event)
		roster = event.players
		for _, watcher in rosterWatchers do
			watcher(event.players)
		end
	end,

	-- A join and a leave run at once, so the leaving player's hero still reads.
	playerJoined = function(event)
		for _, handler in joins do
			handler(event)
		end
	end,

	playerLeft = function(event)
		for _, handler in leaves do
			handler(event)
		end
	end,
}

-- Answer each of the host's events with the handlers above.
host.handle(function(call: string): string
	return serveMod(handlers, call)
end)

return modlock
