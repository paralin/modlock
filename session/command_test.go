package session

import (
	"errors"
	"testing"
)

func TestEncodePlayerCommandRequest(t *testing.T) {
	data := encodePlayerCommandRequest(3, "monopolis_requestdiceroll")
	field, typ, rest, ok := readKey(data)
	if !ok || field != 7 || typ != 2 {
		t.Fatalf("got field %d typ %d, want 7/2", field, typ)
	}
	inner, rest, ok := readBytes(rest)
	if !ok || len(rest) != 0 {
		t.Fatalf("want one length-delimited body, leftover %d", len(rest))
	}
	slot, line, err := readPlayerCommand(inner)
	if err != nil {
		t.Fatal(err)
	}
	if slot != 3 || line != "monopolis_requestdiceroll" {
		t.Fatalf("got slot %d line %q", slot, line)
	}

	// A negative slot is stored as the proto3 int32 varint (two's complement).
	data = encodePlayerCommandRequest(-1, "start")
	_, typ, rest, ok = readKey(data)
	if !ok || typ != 2 {
		t.Fatal("want field 7")
	}
	inner, _, ok = readBytes(rest)
	if !ok {
		t.Fatal("want body")
	}
	slot, line, err = readPlayerCommand(inner)
	if err != nil {
		t.Fatal(err)
	}
	if slot != -1 || line != "start" {
		t.Fatalf("got slot %d line %q", slot, line)
	}
}

func readPlayerCommand(data []byte) (int32, string, error) {
	var slot int32
	var line string
	rest := data
	for len(rest) > 0 {
		field, typ, next, ok := readKey(rest)
		if !ok {
			return 0, "", errTruncated
		}
		rest = next
		switch {
		case field == 1 && typ == 0:
			v, next, ok := readVarint(rest)
			if !ok {
				return 0, "", errTruncated
			}
			slot = int32(v)
			rest = next
		case field == 2 && typ == 2:
			b, next, ok := readBytes(rest)
			if !ok {
				return 0, "", errTruncated
			}
			line = string(b)
			rest = next
		default:
			return 0, "", errTruncated
		}
	}
	return slot, line, nil
}

func readKey(b []byte) (field, typ int, rest []byte, ok bool) {
	v, rest, ok := readVarint(b)
	if !ok {
		return 0, 0, nil, false
	}
	return int(v >> 3), int(v & 7), rest, true
}

func readBytes(b []byte) (data, rest []byte, ok bool) {
	n, rest, ok := readVarint(b)
	if !ok || uint64(len(rest)) < n {
		return nil, nil, false
	}
	return rest[:n], rest[n:], true
}

func readVarint(b []byte) (uint64, []byte, bool) {
	var v uint64
	var s uint
	for i, x := range b {
		v |= uint64(x&0x7f) << s
		if x < 0x80 {
			return v, b[i+1:], true
		}
		s += 7
		if s >= 64 {
			return 0, nil, false
		}
	}
	return 0, nil, false
}

var errTruncated = errors.New("truncated")
