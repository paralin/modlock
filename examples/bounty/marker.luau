--!strict
-- marker floats the holder's name and score over their hero, so everyone can
-- see whom to hunt.
local modlock = require("@modlock")
local vector = require("./vector")

type Player = modlock.Player
type WorldObject = modlock.WorldObject

local marker = {}

-- gold is the marker's color as 0xRRGGBBAA.
local gold = 0xffc23dff

-- text is the marker in the world, and words is the text it shows.
local text: WorldObject? = nil
local words = ""

-- remove takes the marker out of the world.
function marker.remove()
	if text then
		text:remove()
	end
	text = nil
end

-- forget clears the mod's record of the marker. It does not remove the
-- marker from the game, because the game already removed it when it loaded
-- the new map.
function marker.forget()
	text = nil
end

-- follow moves the marker over the holder's hero and shows label. While the
-- holder has no living hero, follow removes the marker, and it raises a new
-- one when they respawn.
function marker.follow(holder: Player, label: string)
	-- Hide the marker while the holder has no living hero.
	local pawn = holder:pawn()
	if pawn == nil or pawn.health <= 0 then
		marker.remove()
		return
	end

	-- Raise a new marker over the hero when none is showing.
	local position = vector.above(pawn.position, 130)
	local current = text
	if current == nil then
		current = modlock.createText({
			text = label,
			position = position,
			fontSize = 70,
			color = gold,
			faceCamera = true,
		})
		text = current
		words = label
		return
	end

	-- Move the marker with the hero, and set its text only when the label changed.
	current:move(position)
	if label ~= words then
		current:setText(label)
		words = label
	end
end

return marker
