package kv3

import (
	"bytes"
	"encoding/binary"
	"math"

	"github.com/pkg/errors"
)

// decoder reads one KeyValues3 buffer's values in order. Each read past the
// end of a lane yields zero and records errTruncated, so a value walk ends
// with one error check.
type decoder struct {
	// strings are the buffer's strings, indexed by id.
	strings []string
	// current are the lanes scalar values read from: main, or auxiliary
	// for the items of an auxiliary array.
	current *[4][]byte
	// main are the lanes that hold string ids, container lengths and the
	// scalars outside auxiliary arrays.
	main *[4][]byte
	// auxiliary are the lanes an auxiliary array reads its scalar items from.
	auxiliary *[4][]byte
	// objects are the remaining object sizes.
	objects []byte
	// inlineObjects reads each object's size from the 4-byte lane instead,
	// as version 4 stores it.
	inlineObjects bool
	// types are the remaining type bytes.
	types []byte
	// blobs are the remaining blob sizes.
	blobs []byte
	// budget is how many more values the buffer may hold.
	budget int
	// err is the first error the walk met.
	err error
}

// take returns the next width bytes of the current lane with the given
// index, or nil once it runs out.
func (d *decoder) take(index int) []byte {
	// A lane shorter than its width has run out.
	lane := &d.current[index]
	width := 1 << index
	if len(*lane) < width {
		d.fail(errTruncated)
		return nil
	}

	// Consume the value.
	value := (*lane)[:width]
	*lane = (*lane)[width:]
	return value
}

// u8 reads the next 1-byte value.
func (d *decoder) u8() uint8 {
	if value := d.take(0); value != nil {
		return value[0]
	}
	return 0
}

// u16 reads the next 2-byte value.
func (d *decoder) u16() uint16 {
	if value := d.take(1); value != nil {
		return binary.LittleEndian.Uint16(value)
	}
	return 0
}

// u32 reads the next 4-byte value.
func (d *decoder) u32() uint32 {
	if value := d.take(2); value != nil {
		return binary.LittleEndian.Uint32(value)
	}
	return 0
}

// u64 reads the next 8-byte value.
func (d *decoder) u64() uint64 {
	if value := d.take(3); value != nil {
		return binary.LittleEndian.Uint64(value)
	}
	return 0
}

// length reads a string id or container length, which always come from the
// main lanes, from the 1-byte lane when short or the 4-byte lane.
func (d *decoder) length(short bool) int {
	saved := d.current
	d.current = d.main
	defer func() { d.current = saved }()
	if short {
		return int(d.u8())
	}
	return int(int32(d.u32()))
}

// cstring reads a null-terminated string from the 1-byte lane.
func (d *decoder) cstring() (string, bool) {
	// The string ends at the lane's next zero byte.
	lane := &d.current[0]
	length := bytes.IndexByte(*lane, 0)
	if length < 0 {
		return "", false
	}

	// Consume the string and its terminator.
	text := string((*lane)[:length])
	*lane = (*lane)[length+1:]
	return text, true
}

// pop takes the next 4-byte size from a table such as the object or blob
// sizes.
func (d *decoder) pop(table *[]byte) int {
	if len(*table) < 4 {
		d.fail(errTruncated)
		return 0
	}
	size := int(int32(binary.LittleEndian.Uint32(*table)))
	*table = (*table)[4:]
	return size
}

// kind reads the next type, skipping its flag and reserved bytes.
func (d *decoder) kind() byte {
	// Each type is one byte; a set high bit adds a flag byte and the next
	// bit a reserved byte.
	if len(d.types) == 0 {
		d.fail(errTruncated)
		return 0
	}
	kind := d.types[0]
	skip := 1
	if kind&0x80 != 0 {
		skip++
	}
	if kind&0x40 != 0 {
		skip++
	}
	if len(d.types) < skip {
		d.fail(errTruncated)
		return 0
	}
	d.types = d.types[skip:]
	return kind & 0x3f
}

// text resolves a string id; -1 names the empty string.
func (d *decoder) text(id int32) string {
	if id == -1 {
		return ""
	}
	if id < 0 || int(id) >= len(d.strings) {
		d.fail(errors.Errorf("KeyValues3 data names missing string %d", id))
		return ""
	}
	return d.strings[id]
}

// value reads one value of kind at the given nesting depth.
func (d *decoder) value(kind byte, depth int) any {
	// Stop at the first error, past the nesting bound, or past the budget.
	if d.err != nil {
		return nil
	}
	if depth > maxDepth {
		d.fail(errors.New("KeyValues3 data nests too deeply"))
		return nil
	}
	if d.budget--; d.budget < 0 {
		d.fail(errors.New("KeyValues3 data holds too many values"))
		return nil
	}

	// Read the scalar or container the type names.
	switch kind {
	case 1:
		return nil
	case 2:
		return d.u8() == 1
	case 3:
		return int64(d.u64())
	case 4:
		return d.u64()
	case 5:
		return math.Float64frombits(d.u64())
	case 6:
		return d.text(int32(d.length(false)))
	case 7:
		return Blob(d.pop(&d.blobs))
	case 8:
		return d.items(d.length(false), 0, depth, d.main)
	case 9:
		return d.object(depth)
	case 10:
		return d.items(d.length(false), d.kind(), depth, d.main)
	case 11:
		return int64(int32(d.u32()))
	case 12:
		return int64(d.u32())
	case 13:
		return true
	case 14:
		return false
	case 15:
		return int64(0)
	case 16:
		return int64(1)
	case 17:
		return float64(0)
	case 18:
		return float64(1)
	case 19:
		return float64(math.Float32frombits(d.u32()))
	case 20:
		return int64(int16(d.u16()))
	case 21:
		return int64(d.u16())
	case 22:
		return int64(int8(d.u8()))
	case 23:
		return int64(d.u8())
	case 24:
		return d.items(d.length(true), d.kind(), depth, d.main)
	case 25:
		// An auxiliary array reads its scalar items from the auxiliary
		// lanes; their strings and containers still read from the main ones.
		length := d.length(true)
		return d.items(length, d.kind(), depth, d.auxiliary)
	case 26:
		return int64(0)
	case 27:
		return int64(1)
	default:
		d.fail(errors.Errorf("Unknown KeyValues3 type %d", kind))
		return nil
	}
}

// items reads an array of length values, each of the type item, or each
// with its own type when item is zero, reading their scalars from lanes.
func (d *decoder) items(length int, item byte, depth int, lanes *[4][]byte) []any {
	if length < 0 || length > d.budget {
		d.fail(errors.New("KeyValues3 data holds too many values"))
		return nil
	}
	saved := d.current
	defer func() { d.current = saved }()
	items := make([]any, 0, length)
	for range length {
		kind := item
		if kind == 0 {
			kind = d.kind()
		}
		d.current = lanes
		items = append(items, d.value(kind, depth+1))
	}
	return items
}

// object reads an object, whose size comes from the object size table, or
// from the 4-byte lane in version 4, and
// whose keys are string ids.
func (d *decoder) object(depth int) map[string]any {
	// The size must fit the value budget.
	var length int
	if d.inlineObjects {
		length = d.length(false)
	} else {
		length = d.pop(&d.objects)
	}
	if length < 0 || length > d.budget {
		d.fail(errors.New("KeyValues3 data holds too many values"))
		return nil
	}

	// Each member is a type, a key and a value, read from the main lanes.
	saved := d.current
	defer func() { d.current = saved }()
	object := make(map[string]any, length)
	for range length {
		d.current = d.main
		kind := d.kind()
		key := d.text(int32(d.length(false)))
		object[key] = d.value(kind, depth+1)
	}
	return object
}

// fail records the first error of the walk.
func (d *decoder) fail(err error) {
	if d.err == nil {
		d.err = err
	}
}
