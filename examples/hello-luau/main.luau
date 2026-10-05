-- hello-luau is the smallest Modlock mod in Luau: it answers /hello in chat
-- and reports the first server frame.
local modlock = require("@modlock")

-- Greet each player who types /hello.
modlock.command("hello", function(player)
	player:chat("Hello from Luau!")
end)

-- Report the first server frame once.
local seen = false
modlock.onFrame(function(frame)
	if seen then
		return
	end
	seen = true
	modlock.log("first frame at tick " .. frame.tick)
end)
