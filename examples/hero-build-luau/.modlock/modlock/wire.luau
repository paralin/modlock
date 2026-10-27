--!strict
-- wire encodes and decodes the protobuf messages of the boundary between
-- the game and a mod, as the generated schema describes them. A message is
-- a table keyed by its fields' lowerCamel names; the schema's rules for each
-- field are on its descriptor.
--
-- Integers are numbers, which hold every value up to 2^53 exactly. A 64-bit
-- id, such as a Steam ID, is a decimal string, because it does not fit.

local wire = {}

-- Field describes one field of a message.
export type Field = {
	-- number is the field's number in the schema.
	number: number,
	-- name is the field's lowerCamel name.
	name: string,
	-- kind is the field's protobuf type, such as "uint32" or "message".
	kind: string,
	-- repeated marks a list of values.
	repeated: boolean?,
	-- optional marks a field whose presence the receiver sees, so the encoder
	-- sends even its zero value.
	optional: boolean?,
	-- oneof names the oneof the field is a case of; the message holds the
	-- case's value under that name.
	oneof: string?,
	-- message names the field's message type.
	message: string?,
	-- union names the oneof of a message that holds only that oneof, which
	-- stands for the value of its case.
	union: string?,
	-- enum maps each value's number to its name and each name to its number.
	enum: { [any]: any }?,
	-- class is the class the field's number addresses, which key holds.
	class: any,
	key: string?,
	-- id marks a 64-bit integer held as a decimal string.
	id: boolean?,
}

-- Schema holds each message's fields by the message's name.
export type Schema = { [string]: { Field } }

-- wireTypes holds the wire type of each kind that is not a varint.
local wireTypes: { [string]: number } = {
	double = 1,
	float = 5,
	fixed32 = 5,
	string = 2,
	bytes = 2,
	message = 2,
}

-- packable reports whether a repeated field of kind packs into one record.
local function packable(kind: string): boolean
	return wireTypes[kind] ~= 2
end

-- luauType returns what typeof answers for a value of field.
local function luauType(field: Field): string
	if field.class or field.kind == "message" then
		return "table"
	elseif field.kind == "bool" then
		return "boolean"
	elseif field.kind == "string" or field.kind == "bytes" or field.enum or field.id then
		return "string"
	end
	return "number"
end

-- readVarint reads the varint at i in s as its low and high 32 bits, and
-- returns them with the position after it.
local function readVarint(s: string, i: number): (number, number, number)
	local lo, hi, shift = 0, 0, 0
	while true do
		-- Read the next byte; the number must end before the message does.
		local b = string.byte(s, i)
		if b == nil then
			error("a message ends inside a number", 0)
		end
		i += 1

		-- Add its seven bits to the low word, the high word or both.
		local bits = bit32.band(b, 0x7f)
		if shift < 32 then
			lo += bit32.lshift(bits, shift)
			if shift > 25 then
				hi += bit32.rshift(bits, 32 - shift)
			end
		elseif shift < 64 then
			hi += bit32.lshift(bits, shift - 32)
		end

		-- A byte without its top bit ends the number.
		shift += 7
		if b < 0x80 then
			return lo, hi, i
		end
	end
end

-- varint returns the varint of the 64-bit integer whose low and high 32 bits
-- are lo and hi.
local function varint(lo: number, hi: number): string
	local bytes = {}
	while hi > 0 or lo >= 0x80 do
		table.insert(bytes, bit32.bor(bit32.band(lo, 0x7f), 0x80))
		lo = bit32.bor(bit32.rshift(lo, 7), bit32.lshift(bit32.band(hi, 0x7f), 25))
		hi = bit32.rshift(hi, 7)
	end
	table.insert(bytes, lo)
	return string.char(table.unpack(bytes))
end

-- split returns the low and high 32 bits of the integer n, in two's
-- complement when n is negative.
local function split(n: number): (number, number)
	local hi = math.floor(n / 4294967296)
	local lo = math.floor(n - hi * 4294967296)
	if hi < 0 then
		hi += 4294967296
	end
	return lo, hi
end

-- join returns the integer whose low and high 32 bits are lo and hi.
local function join(lo: number, hi: number, signed: boolean): number
	if signed and hi >= 0x80000000 then
		return (hi - 4294967296) * 4294967296 + lo
	end
	return hi * 4294967296 + lo
end

-- decimal returns the unsigned 64-bit integer of lo and hi in decimal. It
-- divides the four 16-bit digits of the number by ten until none remain.
local function decimal(lo: number, hi: number): string
	local a, b, c, d = hi // 65536, hi % 65536, lo // 65536, lo % 65536
	local digits = {}
	repeat
		local r = a % 10
		a //= 10
		b += r * 65536
		r = b % 10
		b //= 10
		c += r * 65536
		r = c % 10
		c //= 10
		d += r * 65536
		r = d % 10
		d //= 10
		table.insert(digits, 1, r)
	until a + b + c + d == 0
	return table.concat(digits)
end

-- parse returns the low and high 32 bits of an unsigned 64-bit integer in
-- decimal.
local function parse(text: string): (number, number)
	local a, b, c, d = 0, 0, 0, 0
	for i = 1, #text do
		local digit = string.byte(text, i) - 48
		if digit < 0 or digit > 9 then
			error(string.format("%q is not a decimal id", text), 0)
		end
		d = d * 10 + digit
		c = c * 10 + d // 65536
		d %= 65536
		b = b * 10 + c // 65536
		c %= 65536
		a = (a * 10 + b // 65536) % 65536
		b %= 65536
	end
	return c * 65536 + d, a * 65536 + b
end

-- skip returns the position after a value of wire type kind at i.
local function skip(s: string, i: number, kind: number): number
	if kind == 0 then
		local _, _, after = readVarint(s, i)
		return after
	elseif kind == 1 then
		return i + 8
	elseif kind == 5 then
		return i + 4
	elseif kind == 2 then
		local size, _, after = readVarint(s, i)
		return after + size
	end
	error(string.format("a message holds wire type %d", kind), 0)
end

-- indexes caches each message's fields by number.
local indexes: { [{ Field }]: { [number]: Field } } = setmetatable({}, { __mode = "k" }) :: any

-- index returns fields by number.
local function index(fields: { Field }): { [number]: Field }
	local found = indexes[fields]
	if found == nil then
		found = {}
		for _, field in fields do
			found[field.number] = field
		end
		indexes[fields] = found
	end
	return found
end

-- decode is declared ahead of read, which calls it for a nested message.
local decode: (schema: Schema, name: string, s: string) -> any

-- read returns the value of field at i in s, whose wire type is kind, and
-- the position after it.
local function read(schema: Schema, field: Field, s: string, i: number, kind: number): (any, number)
	-- Read a varint as a bool, an id, an enum's name, a class or a number.
	if kind == 0 then
		local lo, hi, after = readVarint(s, i)
		local value: any
		if field.kind == "bool" then
			value = lo ~= 0 or hi ~= 0
		elseif field.id then
			value = decimal(lo, hi)
		else
			value = join(lo, hi, field.kind == "int32" or field.kind == "int64" or field.enum ~= nil)
			if field.enum then
				value = field.enum[value]
			elseif field.class then
				value = field.class.new(value)
			end
		end
		return value, after
	end

	-- Read the fixed-width kinds.
	if kind == 5 then
		local value: any, after: any = string.unpack(if field.kind == "float" then "<f" else "<I4", s, i)
		if field.class then
			value = field.class.new(value)
		end
		return value, after
	elseif kind == 1 then
		return string.unpack("<d", s, i)
	end

	-- Read a length-delimited value, which must end inside the message.
	local size, _, start = readVarint(s, i)
	local after = start + size
	if after > #s + 1 then
		error("a message ends inside a field", 0)
	end
	local bytes = string.sub(s, start, after - 1)

	-- Return strings and bytes as they are, and decode a nested message.
	if field.kind ~= "message" then
		return bytes, after
	end
	local message = decode(schema, field.message :: string, bytes)
	if field.union then
		return message[field.union], after
	end
	return message, after
end

-- default returns the value of field when a message leaves it out.
local function default(schema: Schema, field: Field): any
	if field.repeated then
		return {}
	elseif field.optional or field.enum or field.union then
		return nil
	elseif field.kind == "message" then
		return decode(schema, field.message :: string, "")
	elseif field.class then
		return field.class.new(0)
	elseif field.id then
		return "0"
	elseif field.kind == "bool" then
		return false
	elseif field.kind == "string" or field.kind == "bytes" then
		return ""
	end
	return 0
end

-- decode returns the message name encoded in s. A repeated enum leaves out
-- values the library has no name for.
function decode(schema: Schema, name: string, s: string): any
	-- Read each field in turn, skipping the ones the schema does not know.
	local fields = schema[name]
	local byNumber = index(fields)
	local out: { [string]: any } = {}
	local i = 1
	while i <= #s do
		local key, _, after = readVarint(s, i)
		i = after
		local kind = bit32.band(key, 7)
		local field = byNumber[bit32.rshift(key, 3)]
		if field == nil then
			i = skip(s, i, kind)
		elseif field.repeated then
			local list = out[field.name] or {}
			out[field.name] = list
			if kind == 2 and packable(field.kind) then
				local size, _, start = readVarint(s, i)
				i = start + size
				local at = start
				while at < i do
					local value
					value, at = read(schema, field, s, at, wireTypes[field.kind] or 0)
					if value ~= nil then
						table.insert(list, value)
					end
				end
			else
				local value
				value, i = read(schema, field, s, i, kind)
				if value ~= nil then
					table.insert(list, value)
				end
			end
		else
			local value
			value, i = read(schema, field, s, i, kind)
			out[field.oneof or field.name] = value
		end
	end

	-- Fill each field the message left out with its default.
	for _, field in fields do
		if field.oneof == nil and out[field.name] == nil then
			out[field.name] = default(schema, field)
		end
	end
	return out
end

-- encode is declared ahead of bytes, which calls it for a nested message.
local encode: (schema: Schema, name: string, value: any) -> string

-- bytes returns the encoded value of field, without its tag.
local function bytes(schema: Schema, field: Field, value: any): string
	if field.class then
		value = value[field.key :: string]
	end
	if field.kind == "message" then
		if field.union then
			value = { [field.union] = value }
		end
		local body = encode(schema, field.message :: string, value)
		return varint(#body, 0) .. body
	elseif field.kind == "string" or field.kind == "bytes" then
		return varint(#value, 0) .. value
	elseif field.kind == "float" then
		return string.pack("<f", value)
	elseif field.kind == "fixed32" then
		return string.pack("<I4", value)
	elseif field.kind == "double" then
		return string.pack("<d", value)
	elseif field.kind == "bool" then
		return if value then "\1" else "\0"
	elseif field.id then
		return varint(parse(tostring(value)))
	elseif field.enum then
		local number = field.enum[value]
		if type(number) ~= "number" then
			error(string.format("%s cannot be %q", field.name, tostring(value)), 0)
		end
		value = number
	end
	return varint(split(value))
end

-- tag returns the key that precedes a field's value on the wire.
local function tag(field: Field, kind: number): string
	return varint(field.number * 8 + kind, 0)
end

-- encode returns the message name holding value. It leaves out fields that
-- are nil, and the zero values the receiver assumes. A oneof's value takes
-- the first case of its Luau type.
function encode(schema: Schema, name: string, value: any): string
	local parts = {}
	local cases: { [string]: boolean } = {}
	for _, field in schema[name] do
		-- Encode the first case of a oneof whose type matches its value.
		local kind = wireTypes[field.kind] or 0
		if field.oneof then
			local case = value[field.oneof]
			if case ~= nil and not cases[field.oneof] and typeof(case) == luauType(field) then
				cases[field.oneof] = true
				table.insert(parts, tag(field, kind) .. bytes(schema, field, case))
			end
			continue
		end

		-- Encode a list packed when its kind allows, else one record per item,
		-- and a single value unless the receiver assumes it.
		local v = value[field.name]
		if v == nil then
			continue
		end
		if field.repeated and packable(field.kind) then
			if #v > 0 then
				local packed = {}
				for _, item in v do
					table.insert(packed, bytes(schema, field, item))
				end
				local body = table.concat(packed)
				table.insert(parts, tag(field, 2) .. varint(#body, 0) .. body)
			end
		elseif field.repeated then
			for _, item in v do
				table.insert(parts, tag(field, kind) .. bytes(schema, field, item))
			end
		elseif field.optional or not (v == false or v == 0 or v == "") then
			table.insert(parts, tag(field, kind) .. bytes(schema, field, v))
		end
	end
	return table.concat(parts)
end

wire.decode = decode
wire.encode = encode

return wire
