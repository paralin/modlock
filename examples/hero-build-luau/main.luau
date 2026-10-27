-- hero-build-luau reads a hero's recommended build from the host's game data.
-- Players type /build hero_inferno to see its categories and items.
local modlock = require("@modlock")

modlock.command("build", function(player, args)
	local hero = if args == "" then "hero_inferno" else args
	local categories = modlock.heroBuild(hero)
	if #categories == 0 then
		player:chat("No build for " .. hero .. ".")
		return
	end
	for _, category in categories do
		local names = {}
		for _, item in category.items do
			table.insert(names, item.item)
		end
		player:chat(category.name .. ": " .. table.concat(names, ", "))
	end
end)
