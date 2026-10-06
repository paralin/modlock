--!strict
-- vector holds the little arithmetic the mod does on positions.
local modlock = require("@modlock")

type Vector = modlock.Vector
type Angles = modlock.Angles

local vector = {}

-- add returns a + b * scale; scale defaults to 1.
function vector.add(a: Vector, b: Vector, scale: number?): Vector
	local s = scale or 1
	return { x = a.x + b.x * s, y = a.y + b.y * s, z = a.z + b.z * s }
end

-- above returns the point height units over position.
function vector.above(position: Vector, height: number): Vector
	return { x = position.x, y = position.y, z = position.z + height }
end

-- forward returns the unit direction that angles look along. A positive
-- pitch looks down, as the game measures it.
function vector.forward(angles: Angles): Vector
	local pitch = math.rad(angles.pitch)
	local yaw = math.rad(angles.yaw)
	return {
		x = math.cos(pitch) * math.cos(yaw),
		y = math.cos(pitch) * math.sin(yaw),
		z = -math.sin(pitch),
	}
end

return vector
