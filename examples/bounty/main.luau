--!strict
-- bounty is a free-for-all built on the stock game. One player at a time
-- holds the bounty: a gold marker floats over their hero, they take less
-- damage, and they score a point for each second they survive. Whoever kills
-- the holder takes the bounty, and while nobody holds it, any kill claims it.
-- The first to the target wins the round and a purse of souls.
--
-- Players type /bounty for the standings, /target to change the winning
-- score, /bots to add practice bots and /newround to start over. Their flare
-- key marks the spot they aim at for everyone. Start the server with
-- "-- 120" after the host's arguments to play to 120 seconds.
local modlock = require("@modlock")
local flare = require("./flare")
local marker = require("./marker")
local roster = require("./roster")
local round = require("./round")

type Player = modlock.Player

-- shield is the share of each hit the holder takes.
local shield = 0.75

-- purse is the souls the round's winner receives.
local purse = 3000

-- lastTime is the game clock at the previous frame, or nil before the first.
local lastTime: number? = nil

-- parseTarget reads a winning score from text, or returns nil when it is
-- not a whole number of seconds from 10 to 600.
local function parseTarget(text: string): number?
	local seconds = tonumber(text)
	if seconds == nil or seconds ~= math.floor(seconds) or seconds < 10 or seconds > 600 then
		return nil
	end
	return seconds
end

-- label is the marker's words for the holder.
local function label(holder: Player): string
	return `BOUNTY\n{roster.name(holder)}  {round.seconds(holder)}/{round.target}`
end

-- callout tells every player who wants it that the bounty changed hands.
local function callout(text: string)
	roster.each(function(player)
		if player:setting("callouts") ~= "false" then
			player:chat(text)
		end
	end)
end

-- award hands the bounty to taker and tells everyone.
local function award(taker: Player)
	round.claim(taker)
	taker:announce("You hold the bounty", `Survive {round.target - round.seconds(taker)} more seconds to win.`)
	taker:sound("Damage.Send.Crit")
	callout(`{roster.name(taker)} holds the bounty. Hunt them down!`)
end

-- finish ends the round with winner and starts the next.
local function finish(winner: Player)
	-- Tell everyone who won.
	local name = roster.name(winner)
	roster.each(function(player)
		player:announce(`{name} wins`, `{name} held the bounty for {round.target} seconds.`)
	end)

	-- Pay the winner and start the next round.
	winner:adjustSouls(purse)
	winner:addMetric("bounty.won", 1)
	marker.remove()
	round.reset()
end

modlock.onStart(function(args)
	-- Play to the target the server's first argument gives, when it is valid.
	local target = if args[1] then parseTarget(args[1]) else nil
	if target then
		round.target = target
	elseif args[1] then
		modlock.log(`ignoring target {args[1]}: give whole seconds from 10 to 600`)
	end

	-- Watch the flare keys for the whole game.
	flare.watch()
	modlock.log(`bounty started; first to {round.target} seconds wins`)
end)

-- A new world has removed every object and reset every hero.
modlock.onWorld(function(map)
	marker.forget()
	flare.forget()
	round.reset()
	lastTime = nil
	modlock.log(`bounty round on {map}`)
end)

modlock.onFrame(function(frame)
	-- Measure this frame on the game clock and put out the old flares.
	local now = frame.timeSeconds
	local dt = if lastTime then math.clamp(now - lastTime, 0, 0.5) else 0
	lastTime = now
	flare.expire(now)

	-- Score the holder and end the round when they reach the target.
	local changed, winner = round.advance(dt)
	if winner then
		finish(winner)
		return
	end

	-- Keep the marker over the holder, or announce that the holder left.
	local holder = round.holding()
	if holder then
		marker.follow(holder, label(holder))
	elseif changed then
		-- The holder left the server.
		marker.remove()
		callout("The bounty is free. The next kill claims it.")
	end
end)

-- The holder shrugs off part of every hit.
modlock.onDamage(function(hit)
	local holder = round.holding()
	local pawn = if holder then holder:pawn() else nil
	if pawn and hit.victim == pawn.entity then
		return { amount = hit.amount * shield }
	end
	return nil
end)

-- A kill of the holder passes the bounty to the killer; while nobody holds
-- it, any kill claims it.
modlock.onDamaged(function(hit)
	-- Find who died and who killed them; a suicide credits no one.
	if hit.healthBefore <= 0 or hit.healthLost < hit.healthBefore then
		return
	end
	local victim = roster.byEntity(hit.victim)
	local killer = roster.byEntity(hit.attacker)
	if killer and victim and killer.slot == victim.slot then
		killer = nil
	end

	-- Pass the bounty on, or free it when the holder died to no player.
	local holder = round.holding()
	if holder and victim and victim.slot == holder.slot then
		marker.remove()
		if killer then
			award(killer)
		else
			round.drop()
			callout(`{roster.name(holder)} fell. The next kill claims the bounty.`)
		end
	elseif holder == nil and killer then
		award(killer)
	end
end)

modlock.onInput(function(player, pressed)
	flare.fire(player, pressed, lastTime or 0)
end)

modlock.command("bounty", function(player)
	local holder = round.holding()
	player:chat(if holder
		then `{roster.name(holder)} holds the bounty. First to {round.target} seconds wins.`
		else `Nobody holds the bounty. The next kill claims it. First to {round.target} seconds wins.`)
	for place, standing in round.standings() do
		if place > 5 then
			break
		end
		player:chat(`{place}. {standing.name}: {standing.seconds}s`)
	end
end)

modlock.command("target", function(player, args)
	local target = parseTarget(args)
	if target == nil then
		player:chat("Type /target and a whole number of seconds from 10 to 600.")
		return
	end
	round.target = target
	callout(`{roster.name(player)} set the bounty target to {target} seconds.`)
end)

-- /bots adds practice bots, alternating teams so both sides have targets.
modlock.command("bots", function(player, args)
	local count = math.clamp(tonumber(args) or 2, 1, 6)
	local added = 0
	for i = 1, count do
		local bot = modlock.addBot({ name = `Bounty Bot {i}`, team = 2 + i % 2 })
		if bot then
			added += 1
		end
	end
	player:chat(`Added {added} of {count} bots.`)
end)

modlock.command("newround", function(player)
	marker.remove()
	round.reset()
	callout(`{roster.name(player)} started a new round. The next kill claims the bounty.`)
end)
