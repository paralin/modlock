// Package persist holds the rules for the records a mod keeps for each
// player through the host's persist service: which record types a manifest
// may declare, and how an update's field operations change a record. A host
// that serves the service applies updates with Apply, so every host agrees
// on what an update does and what it refuses.
package persist

import (
	"fmt"
	"math"
	"regexp"
	"slices"
	"strconv"

	"github.com/paralin/modlock/src/wasm"
	persistpb "github.com/paralin/modlock/src/wasm/persist"
	"github.com/pkg/errors"
)

// Service is the name mods call the persist service by.
const Service = "persist"

const (
	// TypeLimit bounds the record types one mod declares.
	TypeLimit = 8
	// FieldLimit bounds the fields of one record type.
	FieldLimit = 32
	// RecordBytes bounds one record's encoded values.
	RecordBytes = 8 << 10
	// OpLimit bounds the operations of one update.
	OpLimit = 32
	// ManyLimit bounds the players one GetMany reads.
	ManyLimit = 16
	// RecentKeys is how many update keys a record remembers.
	RecentKeys = 64
	// UpdateKeyBytes bounds one update key.
	UpdateKeyBytes = 64
	// TagLimit bounds the members of one tags field.
	TagLimit = 256
	// TagBytes bounds one tag.
	TagBytes = 32
	// StringBytes is a string field's bound when it declares none.
	StringBytes = 64
	// StringLimit bounds what a string field may declare.
	StringLimit = 256
)

// namePattern matches a record type or field name.
var namePattern = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_]{0,31}$`)

// Refusal is the error of a call a host refuses: the call broke a rule of
// its record type or a bound, and the record is unchanged. Its text tells
// the mod's author which.
type Refusal string

// Error returns the refusal's text.
func (r Refusal) Error() string { return string(r) }

// refuse returns a Refusal of the formatted text.
func refuse(format string, args ...any) error {
	return Refusal(fmt.Sprintf(format, args...))
}

// Find returns the record type named name that manifest declares, or nil.
func Find(manifest *wasm.Manifest, name string) *persistpb.RecordType {
	for _, recordType := range manifest.GetPersist() {
		if recordType.GetName() == name {
			return recordType
		}
	}
	return nil
}

// Validate checks the record types manifest declares, and that it names the
// persist service exactly when it declares record types.
func Validate(manifest *wasm.Manifest) error {
	// Check the service and the types go together.
	types := manifest.GetPersist()
	named := slices.ContainsFunc(manifest.GetServices(), func(s *wasm.Service) bool { return s.GetName() == Service })
	switch {
	case named && len(types) == 0:
		return errors.New("mod.json names the persist service but declares no persist record types")
	case !named && len(types) != 0:
		return errors.New(`mod.json declares persist record types, so it also names {"name": "persist"} in services`)
	}

	// Check each type.
	if len(types) > TypeLimit {
		return errors.Errorf("mod.json declares more than %d persist record types", TypeLimit)
	}
	for i, recordType := range types {
		if err := validate(recordType); err != nil {
			return err
		}
		if slices.ContainsFunc(types[:i], func(t *persistpb.RecordType) bool { return t.GetName() == recordType.GetName() }) {
			return errors.Errorf("mod.json declares persist record type %q twice", recordType.GetName())
		}
	}
	return nil
}

// validate checks one declared record type.
func validate(recordType *persistpb.RecordType) error {
	// Check the name and the writers.
	name := recordType.GetName()
	if !namePattern.MatchString(name) {
		return errors.Errorf("persist record type %q name must be up to 32 letters, digits and underscores, starting with a letter", name)
	}
	switch recordType.GetWriters() {
	case persistpb.Writers_WRITERS_SERVERS, persistpb.Writers_WRITERS_PLAYERS:
	default:
		return errors.Errorf("persist record type %q writers must be WRITERS_SERVERS or WRITERS_PLAYERS", name)
	}

	// Check each field.
	fields := recordType.GetFields()
	if len(fields) == 0 || len(fields) > FieldLimit {
		return errors.Errorf("persist record type %q declares 1 to %d fields", name, FieldLimit)
	}
	for field, declared := range fields {
		if !namePattern.MatchString(field) {
			return errors.Errorf("persist record type %q field %q name must be up to 32 letters, digits and underscores, starting with a letter", name, field)
		}
		kind := declared.GetKind()
		if _, ok := persistpb.Field_Kind_name[int32(kind)]; !ok || kind == persistpb.Field_KIND_UNKNOWN {
			return errors.Errorf("persist record type %q field %q kind must be KIND_COUNT, KIND_NUMBER, KIND_MAX, KIND_STRING or KIND_TAGS", name, field)
		}
		if declared.GetMaxBytes() != 0 && (kind != persistpb.Field_KIND_STRING || declared.GetMaxBytes() > StringLimit) {
			return errors.Errorf("persist record type %q field %q maxBytes is for a KIND_STRING field, up to %d", name, field, StringLimit)
		}
	}
	return nil
}

// CheckKey refuses a record key: each player has one record of each type
// until hosts set a bound for keyed records.
func CheckKey(key string) error {
	if key != "" {
		return refuse("persist records take no key yet; leave key empty")
	}
	return nil
}

// Apply returns stored, a record of recordType as a host keeps it or nil for
// one never written, changed by ops. An update whose key is among the
// record's recent keys is left out, and Apply reports it repeated with the
// record unchanged. It refuses the whole update, changing nothing, when an op
// breaks its field's rules or the result passes a bound. stored is not
// modified.
func Apply(recordType *persistpb.RecordType, stored *persistpb.StoredRecord, ops []*persistpb.Op, updateKey string) (*persistpb.StoredRecord, bool, error) {
	// Check the update's bounds and its key.
	if len(ops) == 0 || len(ops) > OpLimit {
		return nil, false, refuse("a persist update makes 1 to %d changes", OpLimit)
	}
	if len(updateKey) > UpdateKeyBytes {
		return nil, false, refuse("a persist update key is up to %d bytes", UpdateKeyBytes)
	}
	if updateKey == "" && hasAdd(ops) {
		return nil, false, refuse("a persist update that adds needs an update key, so a resend counts once")
	}
	next := stored.CloneVT()
	if next == nil {
		next = &persistpb.StoredRecord{}
	}
	if updateKey != "" && slices.Contains(next.GetRecentKeys(), updateKey) {
		return next, true, nil
	}
	if next.Record == nil {
		next.Record = &persistpb.Record{}
	}
	if next.Record.Fields == nil {
		next.Record.Fields = map[string]*persistpb.Value{}
	}

	// Apply each op to its field.
	for _, op := range ops {
		declared := recordType.GetFields()[op.GetField()]
		if declared == nil {
			return nil, false, refuse("persist record type %s declares no field %q", recordType.GetName(), op.GetField())
		}
		value, err := applyOp(declared, next.Record.Fields[op.GetField()], op)
		if err != nil {
			return nil, false, err
		}
		next.Record.Fields[op.GetField()] = value
	}

	// Check the result's size, and remember the key.
	if size := next.Record.SizeVT(); size > RecordBytes {
		return nil, false, refuse("a persist record is up to %d bytes; this update makes it %d", RecordBytes, size)
	}
	if updateKey != "" {
		next.RecentKeys = append(next.RecentKeys, updateKey)
		if extra := len(next.RecentKeys) - RecentKeys; extra > 0 {
			next.RecentKeys = slices.Delete(next.RecentKeys, 0, extra)
		}
	}
	next.Version++
	return next, false, nil
}

// hasAdd reports whether any of ops adds to a field. Every other op leaves
// the same record when applied twice.
func hasAdd(ops []*persistpb.Op) bool {
	for _, op := range ops {
		if _, ok := op.GetOp().(*persistpb.Op_Add); ok {
			return true
		}
	}
	return false
}

// holds reports whether value, as stored, is of a field of kind: a number
// for a count, number or max, text for a string, and tags for tags.
func holds(kind persistpb.Field_Kind, value *persistpb.Value) bool {
	switch value.GetValue().(type) {
	case *persistpb.Value_Number:
		return kind == persistpb.Field_KIND_COUNT || kind == persistpb.Field_KIND_NUMBER || kind == persistpb.Field_KIND_MAX
	case *persistpb.Value_Text:
		return kind == persistpb.Field_KIND_STRING
	case *persistpb.Value_Tags:
		return kind == persistpb.Field_KIND_TAGS
	}
	return false
}

// applyOp returns value, a field's current value or nil, changed by op. It
// refuses an op on a value of another kind than the field's, such as text an
// earlier build stored in a field this build counts, rather than read the
// value as the new kind.
func applyOp(field *persistpb.Field, value *persistpb.Value, op *persistpb.Op) (*persistpb.Value, error) {
	name := op.GetField()
	if value.GetValue() != nil && !holds(field.GetKind(), value) {
		return nil, refuse("persist field %s holds a value of another kind than this release's %s, kept by an earlier release; the record is unchanged", name, kindName(field.GetKind()))
	}
	number := func(v float64) (*persistpb.Value, error) {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return nil, refuse("persist field %s takes a finite number", name)
		}
		if field.GetKind() == persistpb.Field_KIND_COUNT && (v < 0 || v != math.Trunc(v)) {
			return nil, refuse("persist field %s counts whole numbers from zero up", name)
		}
		return &persistpb.Value{Value: &persistpb.Value_Number{Number: v}}, nil
	}
	current := value.GetNumber()
	kind := field.GetKind()
	switch o := op.GetOp().(type) {
	case *persistpb.Op_Add:
		if kind == persistpb.Field_KIND_COUNT || kind == persistpb.Field_KIND_NUMBER {
			if kind == persistpb.Field_KIND_COUNT && (o.Add < 0 || o.Add != math.Trunc(o.Add)) {
				return nil, refuse("persist field %s adds whole numbers from zero up", name)
			}
			return number(current + o.Add)
		}
	case *persistpb.Op_Max:
		if kind == persistpb.Field_KIND_NUMBER || kind == persistpb.Field_KIND_MAX {
			if value.GetValue() == nil || o.Max > current {
				return number(o.Max)
			}
			return value, nil
		}
	case *persistpb.Op_Min:
		if kind == persistpb.Field_KIND_NUMBER {
			if value.GetValue() == nil || o.Min < current {
				return number(o.Min)
			}
			return value, nil
		}
	case *persistpb.Op_Set:
		switch set := o.Set.GetValue().(type) {
		case *persistpb.Value_Number:
			if kind == persistpb.Field_KIND_COUNT || kind == persistpb.Field_KIND_NUMBER {
				return number(set.Number)
			}
		case *persistpb.Value_Text:
			if kind == persistpb.Field_KIND_STRING {
				limit := int(field.GetMaxBytes())
				if limit == 0 {
					limit = StringBytes
				}
				if len(set.Text) > limit {
					return nil, refuse("persist field %s holds up to %d bytes", name, limit)
				}
				return &persistpb.Value{Value: &persistpb.Value_Text{Text: set.Text}}, nil
			}
		}
	case *persistpb.Op_Insert:
		if kind == persistpb.Field_KIND_TAGS {
			if len(o.Insert) == 0 || len(o.Insert) > TagBytes {
				return nil, refuse("persist field %s takes tags of 1 to %d bytes", name, TagBytes)
			}
			members := slices.Clone(value.GetTags().GetMembers())
			at, found := slices.BinarySearch(members, o.Insert)
			if !found {
				if len(members) >= TagLimit {
					return nil, refuse("persist field %s holds up to %d tags", name, TagLimit)
				}
				members = slices.Insert(members, at, o.Insert)
			}
			return &persistpb.Value{Value: &persistpb.Value_Tags{Tags: &persistpb.Tags{Members: members}}}, nil
		}
	case *persistpb.Op_Remove:
		if kind == persistpb.Field_KIND_TAGS {
			members := slices.Clone(value.GetTags().GetMembers())
			if at, found := slices.BinarySearch(members, o.Remove); found {
				members = slices.Delete(members, at, at+1)
			}
			return &persistpb.Value{Value: &persistpb.Value_Tags{Tags: &persistpb.Tags{Members: members}}}, nil
		}
	default:
		return nil, refuse("persist field %s: the change names no operation", name)
	}
	return nil, refuse("persist field %s, a %s, cannot take %s", name, kindName(kind), opName(op))
}

// kindName is a field kind as mod.json writes it.
func kindName(kind persistpb.Field_Kind) string {
	if name, ok := persistpb.Field_Kind_name[int32(kind)]; ok {
		return name
	}
	return strconv.Itoa(int(kind))
}

// opName is the name of op's operation.
func opName(op *persistpb.Op) string {
	switch op.GetOp().(type) {
	case *persistpb.Op_Add:
		return "add"
	case *persistpb.Op_Max:
		return "max"
	case *persistpb.Op_Min:
		return "min"
	case *persistpb.Op_Set:
		if op.GetSet().GetValue() == nil {
			return "an empty set"
		}
		return "this set"
	case *persistpb.Op_Insert:
		return "insert"
	case *persistpb.Op_Remove:
		return "remove"
	}
	return "this change"
}
