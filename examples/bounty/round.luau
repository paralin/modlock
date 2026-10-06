--!strict
-- round keeps one round's score: who holds the bounty, how long each player
-- has held it, and the seconds of holding that win.
local modlock = require("@modlock")
local roster = require("./roster")

type Player = modlock.Player

-- Standing is one player's place in the round.
export type Standing = {
	player: Player,
	name: string,
	seconds: number,
}

local round = {}

-- target is the seconds of holding that win a round.
round.target = 90

-- holder is the player who holds the bounty, or nil when nobody does. The
-- generation tells the holder apart from a later player in the same slot.
local holder: Player? = nil
local holderGeneration = 0

-- scores maps each slot to its seconds holding the bounty this round.
local scores: { [number]: number } = {}

-- holding returns the player who holds the bounty, or nil.
function round.holding(): Player?
	return holder
end

-- seconds returns the whole seconds player has held the bounty this round.
function round.seconds(player: Player): number
	return math.floor(scores[player.slot] or 0)
end

-- claim gives the bounty to player and returns who held it before.
function round.claim(player: Player): Player?
	local previous = holder
	local connection = roster.connection(player.slot)
	holder = player
	holderGeneration = if connection then connection.generation else 0
	player:addMetric("bounty.taken", 1)
	return previous
end

-- drop takes the bounty from whoever holds it.
function round.drop()
	holder = nil
end

-- advance adds dt seconds to the holder's score. It returns true when the
-- holder's whole seconds changed, and the holder once they reach the target.
-- A holder who left drops the bounty.
function round.advance(dt: number): (boolean, Player?)
	local player = holder
	if player == nil then
		return false, nil
	end
	local connection = roster.connection(player.slot)
	if connection == nil or connection.generation ~= holderGeneration then
		holder = nil
		return true, nil
	end

	-- Count each whole second once, both on the board and in the metric.
	local before = scores[player.slot] or 0
	local after = before + dt
	scores[player.slot] = after
	local changed = math.floor(after) ~= math.floor(before)
	if changed then
		player:addMetric("bounty.seconds", 1)
	end
	return changed, if after >= round.target then player else nil
end

-- standings lists the players still on the server, most seconds first.
function round.standings(): { Standing }
	local list: { Standing } = {}
	for _, connection in modlock.players() do
		local slot = connection.player.slot
		table.insert(list, {
			player = connection.player,
			name = connection.name,
			seconds = math.floor(scores[slot] or 0),
		})
	end
	table.sort(list, function(a: Standing, b: Standing): boolean
		return a.seconds > b.seconds
	end)
	return list
end

-- reset starts a new round with nobody holding the bounty. It records each
-- player's best round before clearing the scores.
function round.reset()
	for slot, seconds in scores do
		modlock.Player.new(slot):addMetric("bounty.best", math.floor(seconds))
	end
	table.clear(scores)
	holder = nil
end

return round
