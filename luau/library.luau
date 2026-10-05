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
M.Buttons = {
	-- attack is the primary fire button.
	attack = 1,
	-- use is the interact key.
	use = 2 ^ 5,
	-- melee is the light melee button.
	melee = 2 ^ 11,
	-- reload is the reload key.
	reload = 2 ^ 13,
	-- ability1 to ability3 are the hero's first three ability keys.
	ability1 = 2 ^ 33,
	ability2 = 2 ^ 34,
	ability3 = 2 ^ 35,
	-- parry is the melee parry button.
	parry = 2 ^ 42,
}

-- Layers names collision layers for trace, as bits.
M.Layers = {
	-- solid is the world and solid objects.
	solid = 1,
	-- hitbox is the hit boxes of heroes and units.
	hitbox = 2 ^ 1,
	-- trigger is invisible volumes that react to touch.
	trigger = 2 ^ 2,
	-- playerClip is walls that only stop heroes.
	playerClip = 2 ^ 4,
	-- worldGeometry is the map's static geometry.
	worldGeometry = 2 ^ 14,
	-- player is player-controlled heroes.
	player = 2 ^ 18,
	-- npc is units that are not players.
	npc = 2 ^ 19,
	-- hero, trooper and building are those units' bodies.
	hero = 2 ^ 37,
	trooper = 2 ^ 38,
	building = 2 ^ 40,
}

-- has reports whether bits, a sum of Buttons or Layers bits, holds bit.
-- bit32 reaches only the low 32 bits, and these bits go up to 2^42.
function M.has(bits: number, bit: number): boolean
	return bits // bit % 2 == 1
end

-- Service answers the host's calls to one service the mod serves: the
-- method's name and payload in, the answer out. An error becomes the call's
-- error.
export type Service = (method: string, payload: string) -> string

-- commands maps each registered command name to its handler.
local commands: { [string]: (player: Player, args: string) -> () } = {}

-- services maps each service the mod serves to its handler.
local services: { [string]: Service } = {}

-- Each list runs in registration order.
local starts: { (args: { string }) -> () } = {}
local frames: { (frame: FrameEvent) -> () } = {}
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

-- serve answers the host's calls to service with handler, replacing its
-- earlier handler.
function M.serve(service: string, handler: Service)
	services[service] = handler
end

-- command calls handler when a player types /name in chat or their client
-- sends the console command name to the server. The handler receives the
-- text after the name, trimmed of surrounding spaces. Registering a name
-- again replaces its handler.
function M.command(name: string, handler: (player: Player, args: string) -> ())
	commands[name] = handler
end

-- onStart calls handler once when the server starts the mod, with the
-- arguments that follow -- on the modlock-host command line.
function M.onStart(handler: (args: { string }) -> ())
	table.insert(starts, handler)
end

-- onFrame calls handler once per server frame.
function M.onFrame(handler: (frame: FrameEvent) -> ())
	table.insert(frames, handler)
end

-- onWorld calls handler with the map's name each time a world loads, and at
-- start when one already has. Objects and bots from an earlier world are
-- gone by then.
function M.onWorld(handler: (map: string) -> ())
	table.insert(worlds, handler)
end

-- onInput calls handler with the watched buttons a player pressed and
-- released, as Buttons bits, before the next frame. A button is held from
-- its press until its release.
function M.onInput(handler: (player: Player, pressed: number, released: number) -> ())
	table.insert(inputs, handler)
end

-- onRestored calls handler when a player's Player.restoreHero ends: error is
-- nil once the target held for a second, or says why it did not.
function M.onRestored(handler: (player: Player, error: string?) -> ())
	table.insert(restoreds, handler)
end

-- onNpcsRestored calls handler when restoreNpcs ends: error is nil once the
-- map holds every target, or says why it does not.
function M.onNpcsRestored(handler: (error: string?) -> ())
	table.insert(npcsRestoreds, handler)
end

-- onDamage calls handler before each hit lands, so it can block the hit or
-- change its damage. Later handlers see the earlier ones' amount.
function M.onDamage(handler: (hit: DamageEvent) -> DamageResult?)
	table.insert(damages, handler)
end

-- onDamaged calls handler on the frame after each hit lands.
function M.onDamaged(handler: (hit: DamagedEvent) -> ())
	table.insert(damageds, handler)
end

-- onLaunch calls handler with each watched projectile's first frame, before
-- the next frame. watchProjectiles names the projectiles.
function M.onLaunch(handler: (projectile: LaunchEvent) -> ())
	table.insert(launches, handler)
end

-- onImpact calls handler when a watched projectile strikes something, inside
-- the game's impact: its calls apply before the game continues.
function M.onImpact(handler: (impact: ImpactEvent) -> ())
	table.insert(impacts, handler)
end

-- onLanded calls handler when a hero lands under the manifest's QuakeWorld
-- movement, before the next frame.
function M.onLanded(handler: (landing: LandedEvent) -> ())
	table.insert(landeds, handler)
end

-- onSettingChanged calls handler when a player's setting changes outside the
-- mod, such as on the player's profile. The mod's own player:setSetting calls
-- do not reach it.
function M.onSettingChanged(handler: (player: Player, key: string, value: string) -> ())
	table.insert(settingChanges, handler)
end

-- handlers delivers each event to the handlers the mod registered.
local handlers: ModHandlers = {
	start = function(event)
		for _, handler in starts do
			handler(event.args)
		end
		return { frames = #frames ~= 0, damage = #damages ~= 0, damaged = #damageds ~= 0 }
	end,

	frame = function(event)
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

		-- Run the handler and claim the command.
		handler(event.player, args or "")
		return { claimed = true }
	end,

	world = function(event)
		for _, handler in worlds do
			handler(event.map)
		end
	end,

	-- Luau mods build no interface yet, so no button reports a press.
	uiPress = function(_event) end,

	serve = function(event)
		local handler = services[event.service]
		if handler == nil then
			error("the mod serves no service " .. event.service, 0)
		end
		return { payload = handler(event.method, event.payload) }
	end,

	damage = function(event)
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
}

host.handle(function(call: string): string
	return serveMod(handlers, call)
end)

return M
