--!strict
-- roster answers questions about the players on the server. Every answer
-- reads the server afresh, because players join, leave and respawn between
-- any two events.
local modlock = require("@modlock")

type Player = modlock.Player
type Connection = modlock.Connection

local roster = {}

-- connection returns the connection in slot, or nil when the slot is empty.
function roster.connection(slot: number): Connection?
	for _, connection in modlock.players() do
		if connection.player.slot == slot then
			return connection
		end
	end
	return nil
end

-- name returns the player's name, or their slot once they have left.
function roster.name(player: Player): string
	local connection = roster.connection(player.slot)
	return if connection then connection.name else `player {player.slot}`
end

-- byEntity returns the player whose hero is entity, or nil when entity is
-- not a hero, such as a trooper or the world.
function roster.byEntity(entity: number): Player?
	if entity == 0 then
		return nil
	end
	for _, connection in modlock.players() do
		local pawn = connection.player:pawn()
		if pawn and pawn.entity == entity then
			return connection.player
		end
	end
	return nil
end

-- each calls fn with every player who has finished joining.
function roster.each(fn: (player: Player) -> ())
	for _, connection in modlock.players() do
		if connection.ready then
			fn(connection.player)
		end
	end
end

return roster
