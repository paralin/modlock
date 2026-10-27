"""wire encodes and decodes the protobuf messages of the boundary between the
game and a mod, as the generated schema describes them. A message is an
instance of its generated class, whose attributes are the fields' names in
the schema; the schema's rules for each field are on its descriptor."""

import struct
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, cast


@dataclass(frozen=True, slots=True)
class Field:
    """Field describes one field of a message."""

    # number is the field's number in the schema.
    number: int
    # name is the field's name, and the attribute that holds it.
    name: str
    # kind is the field's protobuf type, such as "uint32" or "message".
    kind: str
    repeated: bool = False
    optional: bool = False
    # oneof names the oneof the field is a case of; the message holds the
    # case's value under that name.
    oneof: str | None = None
    # message names the field's message type.
    message: str | None = None
    # union names the oneof of a message that holds only that oneof, which
    # stands for the value of its case.
    union: str | None = None
    # enum maps each value's number to its name and each name to its number.
    enum: Mapping[Any, Any] | None = None
    # cls is the class the field's number addresses, which key holds.
    cls: type[Any] | None = None
    key: str | None = None


# Schema holds each message's class and fields by the message's name. A
# message without a class decodes to a dict.
type Schema = Mapping[str, tuple[type[Any] | None, list[Field]]]

# WIRE_TYPES holds the wire type of each kind that is not a varint.
WIRE_TYPES = {"double": 1, "float": 5, "fixed32": 5, "string": 2, "bytes": 2, "message": 2}

# SIGNED holds the kinds whose varints are two's complement.
SIGNED = {"int32", "int64", "enum"}


def _packable(kind: str) -> bool:
    """_packable reports whether a repeated field of kind packs into one
    record."""
    return WIRE_TYPES.get(kind) != 2


def _varint(value: int) -> bytes:
    """_varint returns the varint of value, in two's complement when it is
    negative."""
    value &= 0xFFFFFFFFFFFFFFFF
    out = bytearray()
    while value >= 0x80:
        out.append(value & 0x7F | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def _read_varint(data: bytes, i: int) -> tuple[int, int]:
    """_read_varint reads the varint at i in data and returns it with the
    position after it."""
    value = shift = 0
    while True:
        if i >= len(data):
            raise ValueError("a message ends inside a number")
        b = data[i]
        i += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            return value & 0xFFFFFFFFFFFFFFFF, i


def _skip(data: bytes, i: int, kind: int) -> int:
    """_skip returns the position after a value of wire type kind at i."""
    if kind == 0:
        return _read_varint(data, i)[1]
    if kind == 1:
        return i + 8
    if kind == 5:
        return i + 4
    if kind == 2:
        size, i = _read_varint(data, i)
        return i + size
    raise ValueError(f"a message holds wire type {kind}")


def _matches(schema: Schema, field: Field, value: object) -> bool:
    """_matches reports whether value can be field's case of a oneof."""
    if field.cls is not None:
        return isinstance(value, field.cls)
    if field.kind == "message" and field.union is None:
        cls = schema[field.message or ""][0]
        return isinstance(value, cls or dict)
    if isinstance(value, bool) or field.kind == "bool":
        return isinstance(value, bool) and field.kind == "bool"
    if field.kind == "string" or field.enum is not None:
        return isinstance(value, str)
    if field.kind == "bytes":
        return isinstance(value, bytes)
    if field.kind in ("double", "float"):
        return isinstance(value, (int, float))
    return isinstance(value, int)


def _get(value: Any, name: str) -> Any:
    """_get returns the attribute name of a message, or None."""
    if isinstance(value, Mapping):
        return cast(Mapping[str, Any], value).get(name)
    return getattr(value, name, None)


def _read(schema: Schema, field: Field, data: bytes, i: int, kind: int) -> tuple[Any, int]:
    """_read returns the value of field at i in data, whose wire type is kind,
    and the position after it."""
    if kind == 0:
        number, i = _read_varint(data, i)
        if field.kind == "bool":
            return number != 0, i
        if field.kind in SIGNED and number >= 1 << 63:
            number -= 1 << 64
        if field.kind == "int32" or field.kind == "uint32" or field.enum is not None:
            number &= 0xFFFFFFFF
            if field.kind != "uint32" and number >= 1 << 31:
                number -= 1 << 32
        if field.enum is not None:
            return field.enum.get(number), i
        if field.cls is not None:
            return field.cls(number), i
        return number, i
    if kind == 5:
        (value,) = struct.unpack_from("<f" if field.kind == "float" else "<I", data, i)
        if field.cls is not None:
            value = field.cls(value)
        return value, i + 4
    if kind == 1:
        return struct.unpack_from("<d", data, i)[0], i + 8
    size, start = _read_varint(data, i)
    after = start + size
    if after > len(data):
        raise ValueError("a message ends inside a field")
    body = data[start:after]
    if field.kind == "string":
        return body.decode(), after
    if field.kind != "message":
        return body, after
    message = decode(schema, field.message or "", body)
    if field.union is not None:
        return message[field.union], after
    return message, after


def _default(schema: Schema, field: Field) -> Any:
    """_default returns the value of field when a message leaves it out."""
    if field.repeated:
        return []
    if field.optional or field.enum is not None or field.union is not None:
        return None
    if field.kind == "message":
        return decode(schema, field.message or "", b"")
    if field.cls is not None:
        return field.cls(0)
    if field.kind == "bool":
        return False
    if field.kind == "string":
        return ""
    if field.kind == "bytes":
        return b""
    if field.kind in ("double", "float"):
        return 0.0
    return 0


def decode(schema: Schema, name: str, data: bytes) -> Any:
    """decode returns the message name encoded in data. A repeated enum
    leaves out values the library has no name for."""
    cls, fields = schema[name]
    by_number = {field.number: field for field in fields}
    out: dict[str, Any] = {}
    i = 0
    while i < len(data):
        key, i = _read_varint(data, i)
        kind = key & 7
        field = by_number.get(key >> 3)
        if field is None:
            i = _skip(data, i, kind)
        elif field.repeated:
            items: list[Any] = out.setdefault(field.name, [])
            if kind == 2 and _packable(field.kind):
                size, at = _read_varint(data, i)
                i = at + size
                while at < i:
                    value, at = _read(schema, field, data, at, WIRE_TYPES.get(field.kind, 0))
                    if value is not None:
                        items.append(value)
            else:
                value, i = _read(schema, field, data, i, kind)
                if value is not None:
                    items.append(value)
        else:
            out[field.oneof or field.name], i = _read(schema, field, data, i, kind)
    for field in fields:
        if field.oneof is None and field.name not in out:
            out[field.name] = _default(schema, field)
    return cls(**out) if cls is not None else out


def _bytes(schema: Schema, field: Field, value: Any) -> bytes:
    """_bytes returns the encoded value of field, without its tag."""
    if field.cls is not None:
        value = getattr(value, field.key or "")
    if field.kind == "message":
        if field.union is not None:
            value = {field.union: value}
        body = encode(schema, field.message or "", value)
        return _varint(len(body)) + body
    if field.kind == "string":
        body = str(value).encode()
        return _varint(len(body)) + body
    if field.kind == "bytes":
        return _varint(len(value)) + bytes(value)
    if field.kind == "float":
        return struct.pack("<f", value)
    if field.kind == "fixed32":
        return struct.pack("<I", value)
    if field.kind == "double":
        return struct.pack("<d", value)
    if field.kind == "bool":
        return b"\1" if value else b"\0"
    if field.enum is not None:
        number = field.enum.get(value)
        if not isinstance(number, int):
            raise ValueError(f"{field.name} cannot be {value!r}")
        value = number
    return _varint(int(value))


def _tag(field: Field, kind: int) -> bytes:
    """_tag returns the key that precedes a field's value on the wire."""
    return _varint(field.number << 3 | kind)


def _zero(value: Any) -> bool:
    """_zero reports whether value is the zero value the receiver assumes."""
    return value is False or (not isinstance(value, bool) and value in (0, "", b""))


def encode(schema: Schema, name: str, value: Any) -> bytes:
    """encode returns the message name holding value, which may be None for
    an empty message. It leaves out fields that are None, and the zero values
    the receiver assumes. A oneof's value takes the first case its type
    matches."""
    if value is None:
        return b""
    parts: list[bytes] = []
    cases: set[str] = set()
    for field in schema[name][1]:
        kind = WIRE_TYPES.get(field.kind, 0)
        if field.oneof is not None:
            case = _get(value, field.oneof)
            if case is not None and field.oneof not in cases and _matches(schema, field, case):
                cases.add(field.oneof)
                parts.append(_tag(field, kind) + _bytes(schema, field, case))
            continue
        v = _get(value, field.name)
        if v is None:
            continue
        if field.repeated and _packable(field.kind):
            if v:
                body = b"".join(_bytes(schema, field, item) for item in v)
                parts.append(_tag(field, 2) + _varint(len(body)) + body)
        elif field.repeated:
            for item in v:
                parts.append(_tag(field, kind) + _bytes(schema, field, item))
        elif field.optional or not _zero(v):
            parts.append(_tag(field, kind) + _bytes(schema, field, v))
    return b"".join(parts)
