-- Tests of the Luau library's handler timing, run by scripts/test-luau.ts
-- against a copy of the generated module that takes its host, and its clock,
-- from ./host.

local wire = require("./modlock/wire")

-- The stand-in host answers every host call with an empty reply, keeps the
-- library's event function and counts the reads of its clock.
local host: any = require("./host")
local reads = 0
local fake: number? = nil
host.hostCall = function(_: string): string
	return ""
end
function host.handle(handler: (call: string) -> string)
	host.handler = handler
end
function host.clock(): number
	reads += 1
	return fake or os.clock()
end
local modlock = require("./modlock")

-- schema holds the messages the tests send and read.
local schema: any = {
	Call = { { number = 1, name = "method", kind = "string" }, { number = 2, name = "request", kind = "bytes" } },
	Reply = { { number = 1, name = "error", kind = "string" }, { number = 2, name = "response", kind = "bytes" } },
	FrameEvent = { { number = 2, name = "timeSeconds", kind = "double" } },
	CommandEvent = { { number = 1, name = "player", kind = "int32" }, { number = 2, name = "line", kind = "string" } },
	WorldEvent = { { number = 1, name = "map", kind = "string" } },
	ServiceCall = {
		{ number = 1, name = "service", kind = "string" },
		{ number = 2, name = "method", kind = "string" },
	},
	ServiceReply = { { number = 1, name = "payload", kind = "bytes" } },
}

-- deliver sends the mod one host event and returns its decoded reply.
local function deliver(method: string, kind: string, request: any): any
	local call = wire.encode(schema, "Call", { method = method, request = wire.encode(schema, kind, request) })
	local reply = wire.decode(schema, "Reply", host.handler(call))
	assert(reply.error == "", reply.error)
	return reply
end

-- profile calls the profiler's service with method and returns its answer.
local function profile(method: string): string
	local reply = deliver("Serve", "ServiceCall", { service = "modlock.profile", method = method })
	return wire.decode(schema, "ServiceReply", reply.response).payload
end

-- The mod's handlers, each defined on its own line of this file.
local frames, ticks, commands = 0, 0, 0
local spend = 0
local line = debug.info(1, "l")
modlock.onFrame(function() frames += 1 end)
modlock.every(0.01, function() ticks += 1 end)
modlock.command("go", function() commands += 1 end)
modlock.onWorld(function() fake = (fake :: number) + spend / 1000 end)

-- check stops the test with message unless ok.
local function check(ok: boolean, message: string)
	if not ok then
		error(message, 2)
	end
end

-- Without the profiler, the library reads no clock.
for i = 1, 5 do
	deliver("Command", "CommandEvent", { player = 0, line = "go" })
	deliver("Frame", "FrameEvent", { timeSeconds = i })
end
check(reads == 0, `read the clock {reads} times without the profiler`)
check(frames == 5 and commands == 5 and ticks > 0, `ran {frames} frames, {commands} commands, {ticks} ticks`)

-- The profiler times each handler by where its function is defined.
check(string.find(profile("read"), '"sites":[]', 1, true) ~= nil, "the first read reported sites")
for i = 10, 13 do
	deliver("Command", "CommandEvent", { player = 0, line = "go" })
	deliver("Frame", "FrameEvent", { timeSeconds = i })
end
local read = profile("read")
print(read)
for _, want in {
	`onFrame","at":"[^"]*profile_test.luau:{line + 1}","calls":4,`,
	`every","at":"[^"]*profile_test.luau:{line + 2}","calls":4,`,
	`command /go","at":"[^"]*profile_test.luau:{line + 3}","calls":4,`,
	'"frames":{"kind":"frame","at":"","calls":4,',
} do
	check(string.find(read, want) ~= nil, `the read lacks {want}`)
end

-- The profiler lists the first sixteen events over 4 ms and counts the rest.
fake = 0
for i = 1, 22 do
	spend = if i == 1 then 6 else 5
	deliver("World", "WorldEvent", { map = "dl_midtown" })
end
read = profile("read")
check(string.find(read, '"slow":[{"event":"World","ms":6.000000,"kind":"onWorld","at":"', 1, true) ~= nil, `slow: {read}`)
check(string.find(read, `profile_test.luau:{line + 4}","siteMs":6.000000}`, 1, true) ~= nil, `slow site: {read}`)
check(select(2, string.gsub(read, '"event":', "")) == 16, `listed other than 16: {read}`)
check(string.find(read, '"slowMore":6}', 1, true) ~= nil, `slowMore: {read}`)
check(string.find(profile("read"), '"slow":[]', 1, true) ~= nil, "a read kept the slow events")

-- Stop turns timing off again.
profile("stop")
fake = nil
reads = 0
deliver("Frame", "FrameEvent", { timeSeconds = 14 })
check(reads == 0, `read the clock {reads} times after stop`)
print("ok")
