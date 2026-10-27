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

-- Service answers the host's calls to one service the mod serves: the
-- method's name and payload in, the answer out. An error becomes the call's
-- error.
export type Service = (method: string, payload: string) -> string

-- commands maps each registered command name to its handler.
local commands: { [string]: (player: Player, args: string) -> () } = {}

-- services maps each service the mod serves to its handler.
local services: { [string]: Service } = {}

-- starts to settingChanges hold each event's handlers, which run in
-- registration order.
local starts: { (args: { string }) -> () } = {}
local frames: { (frame: FrameEvent) -> () } = {}
local chats: { (line: ChatEvent) -> () } = {}
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

-- serve answers the host's calls to service with handler, replacing its
-- earlier handler.
function modlock.serve(service: string, handler: Service)
	services[service] = handler
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
-- only its own timer.
local function advanceTimers(seconds: number)
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
		return { frames = true, damage = #damages ~= 0, damaged = #damageds ~= 0, chat = #chats ~= 0 }
	end,

	frame = function(event)
		advanceTimers(event.timeSeconds)
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

	-- Luau mods build no interface yet, so no button reports a press and no
	-- input a change.
	uiPress = function(_event) end,
	uiInput = function(_event) end,

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
	end,
}

-- Answer each of the host's events with the handlers above.
host.handle(function(call: string): string
	return serveMod(handlers, call)
end)

return modlock
