--!strict
-- flare lets a player mark the spot they aim at with a short-lived sign, so
-- hunters can point the holder out to each other. Each player picks the key
-- in the flare setting, or turns it off.
local modlock = require("@modlock")
local roster = require("./roster")
local vector = require("./vector")

type Player = modlock.Player
type WorldObject = modlock.WorldObject

local flare = {}

-- keys maps each choice of the flare setting to its button.
local keys: { [string]: number } = {
	reload = modlock.Buttons.reload,
	use = modlock.Buttons.use,
}

-- reach is how far a flare can land, and lifetime how long it shows.
local reach = 6000
local lifetime = 4

-- cooldown is the seconds between one player's flares.
local cooldown = 2

-- Sign is one flare in the world and the game time it goes out.
type Sign = { text: WorldObject, expires: number }

local signs: { Sign } = {}

-- ready maps each slot to the game time its next flare is allowed.
local ready: { [number]: number } = {}

-- watch asks the server to report every key a flare can use.
function flare.watch()
	local buttons = 0
	for _, button in keys do
		buttons += button
	end
	modlock.watchInput(buttons)
end

-- fire lights a flare where player aims, when they pressed their flare key
-- and their cooldown has passed. now is the game clock in seconds.
function flare.fire(player: Player, pressed: number, now: number)
	local key = keys[player:setting("flare") or "reload"]
	if key == nil or not modlock.has(pressed, key) or now < (ready[player.slot] or 0) then
		return
	end
	local pawn = player:pawn()
	if pawn == nil or pawn.health <= 0 then
		return
	end

	-- Trace along the hero's aim, through the hero itself.
	local hit = modlock.trace({
		start = pawn.eyePosition,
		["end"] = vector.add(pawn.eyePosition, vector.forward(pawn.eyeAngles), reach),
		layers = modlock.Layers.solid,
		ignore = { pawn.entity },
	})
	if hit == nil or hit.startSolid then
		return
	end

	-- Raise the sign a little off the surface so it does not sink into it.
	local text = modlock.createText({
		text = `▼ {roster.name(player)}`,
		position = vector.add(hit.position, hit.normal, 24),
		fontSize = 50,
		color = 0x5ad1ffff,
		faceCamera = true,
	})
	if text == nil then
		return
	end
	table.insert(signs, { text = text, expires = now + lifetime })
	ready[player.slot] = now + cooldown
	player:sound("Damage.Send.Crit")
end

-- expire removes the signs whose time has passed at now.
function flare.expire(now: number)
	for i = #signs, 1, -1 do
		if signs[i].expires <= now then
			signs[i].text:remove()
			table.remove(signs, i)
		end
	end
end

-- forget drops every sign; a new world has already removed them.
function flare.forget()
	table.clear(signs)
	table.clear(ready)
end

return flare
