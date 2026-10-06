--!strict
--!nolint BuiltinGlobalWrite
--!nolint FunctionUnused
-- The prelude defines require over the mod's sources and runs the main
-- module. It receives the host bridge, which only modules of the modlock
-- library receive in turn.
--
-- require follows the Luau resolution rules: a module is its path without the
-- extension, and an init file stands for its directory. "./x" names a sibling,
-- "../x" a sibling of the parent, "@self/x" a child, and "@modlock" the
-- library. It replaces the global require, which the sandbox lacks, and
-- reads the calling module's name with debug.info, which mod code must not
-- use.

-- host is the bridge the runtime hands the prelude.
local host = ...

-- suffixes are the endings a module's file may have, in the order tried.
local suffixes = { ".luau", ".lua", "/init.luau", "/init.lua" }

-- loaded caches each module's result by file; loading marks the files whose
-- modules are running, to report a cycle instead of recursing.
local loaded: { [string]: any } = {}
local loading: { [string]: boolean } = {}

-- parent returns the directory part of path, or "" at the root.
local function parent(path: string): string
	return string.match(path, "^(.*)/[^/]*$") or ""
end

-- join appends rest to base, resolving "." and ".." segments.
local function join(base: string, rest: string): string
	local parts = {}
	for part in string.gmatch(base .. "/" .. rest, "[^/]+") do
		if part == ".." then
			table.remove(parts)
		elseif part ~= "." then
			table.insert(parts, part)
		end
	end
	return table.concat(parts, "/")
end

-- identity names the module of file: its path without the extension, or its
-- directory for an init file.
local function identity(file: string): string
	local module = string.gsub(file, "%.luau?$", "")
	return (string.gsub(module, "/?init$", ""))
end

-- resolve returns the module that spec names from the module caller.
local function resolve(caller: string, spec: string): string
	if spec == "@modlock" or string.sub(spec, 1, 9) == "@modlock/" then
		return spec
	end
	if string.sub(spec, 1, 6) == "@self/" then
		return join(caller, string.sub(spec, 7))
	end
	if string.sub(spec, 1, 2) == "./" or string.sub(spec, 1, 3) == "../" then
		return join(parent(caller), spec)
	end
	error(string.format("require(%q) needs a path starting with ./, ../ or @", spec), 3)
end

-- use runs the module and returns its result, once per file.
local function use(module: string, spec: string): any
	for _, suffix in suffixes do
		-- Return a module that already ran, and skip a file the mod lacks.
		local file = module .. suffix
		if loaded[file] ~= nil then
			return loaded[file]
		end
		if host.source(file) then
			-- Refuse a cycle and load the file's chunk.
			if loading[file] then
				error(string.format("require(%q) is a cycle back to %s", spec, file), 3)
			end
			local chunk, why = host.load(file)
			if not chunk then
				error(why, 0)
			end

			-- Run the module once, handing the host only to the library's own
			-- modules, and cache its result.
			loading[file] = true
			local result = if string.sub(file, 1, 9) == "@modlock/" then chunk(host) else chunk()
			loading[file] = nil
			loaded[file] = if result == nil then true else result
			return loaded[file]
		end
	end
	error(string.format("require(%q) finds no module %s", spec, module), 3)
end

-- require resolves spec from the calling module, whose file debug.info
-- names without its chunk name's "@".
function require(spec: string): any
	return use(resolve(identity(debug.info(2, "s")), spec), spec)
end

-- Run the mod's main module.
use("main", "main")
