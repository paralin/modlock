// Package records holds the rules for the records a mod declares in its
// manifest: which kinds a build may declare, which fields a record keeps, and
// which field types a later release may change. The library's sandbox, a
// game server and a backend that stores records all check through it, so
// they agree on every record.
package records

import (
	"math"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/paralin/modlock/src/wasm"
	recordspb "github.com/paralin/modlock/src/wasm/records"
	"github.com/pkg/errors"
)

// Service names the host service that takes a mod's records.
const Service = "records"

const (
	// kindLimit bounds the kinds a mod declares.
	kindLimit = 16
	// fieldLimit bounds the fields of one kind.
	fieldLimit = 32
	// valueLimit bounds the texts a TYPE_TEXT field allows.
	valueLimit = 64
	// TextLimit bounds a text value, in bytes.
	TextLimit = 64
)

var (
	// namePattern matches a kind's or a field's name, which is a SQL
	// identifier and a Python keyword argument.
	namePattern = regexp.MustCompile(`^[a-z][a-z0-9_]{0,31}$`)

	// Reserved are the columns a host adds to every row, which no field may
	// take.
	Reserved = []string{"id", "at", "version", "game", "seq", "lobby", "tier"}
)

// Find returns the kind named name that manifest declares, or nil.
func Find(manifest *wasm.Manifest, name string) *recordspb.RecordKind {
	for _, kind := range manifest.GetRecords() {
		if kind.GetName() == name {
			return kind
		}
	}
	return nil
}

// Validate checks the record kinds manifest declares.
func Validate(manifest *wasm.Manifest) error {
	kinds := manifest.GetRecords()
	if len(kinds) > kindLimit {
		return errors.Errorf("mod.json declares more than %d kinds of record", kindLimit)
	}
	for i, kind := range kinds {
		if err := validateKind(kind); err != nil {
			return err
		}
		if slices.ContainsFunc(kinds[:i], func(k *recordspb.RecordKind) bool { return k.GetName() == kind.GetName() }) {
			return errors.Errorf("mod.json declares record %q twice", kind.GetName())
		}
	}
	return nil
}

// validateKind checks one declared kind and its fields.
func validateKind(kind *recordspb.RecordKind) error {
	// Check the kind's name and how many fields it has.
	name := kind.GetName()
	if !namePattern.MatchString(name) {
		return errors.Errorf("record %q name must be a lowercase letter followed by up to 31 lowercase letters, digits and underscores", name)
	}
	fields := kind.GetFields()
	if len(fields) > fieldLimit {
		return errors.Errorf("record %q declares more than %d fields", name, fieldLimit)
	}

	// Check each field.
	for i, field := range fields {
		if err := validateField(field); err != nil {
			return errors.Wrapf(err, "record %q", name)
		}
		if slices.ContainsFunc(fields[:i], func(f *recordspb.RecordField) bool { return f.GetName() == field.GetName() }) {
			return errors.Errorf("record %q declares field %q twice", name, field.GetName())
		}
	}
	return nil
}

// validateField checks one declared field.
func validateField(field *recordspb.RecordField) error {
	// Check the name, which may not take a host column.
	name := field.GetName()
	if !namePattern.MatchString(name) {
		return errors.Errorf("field %q name must be a lowercase letter followed by up to 31 lowercase letters, digits and underscores", name)
	}
	if slices.Contains(Reserved, name) {
		return errors.Errorf("field %q takes a name the host adds to every record (%s)", name, strings.Join(Reserved, ", "))
	}

	// Check the type, and the allowed texts only a text field lists.
	values := field.GetValues()
	switch field.GetType() {
	case recordspb.RecordField_TYPE_TEXT:
	case recordspb.RecordField_TYPE_INTEGER, recordspb.RecordField_TYPE_NUMBER, recordspb.RecordField_TYPE_BOOL, recordspb.RecordField_TYPE_PLAYER:
		if len(values) != 0 {
			return errors.Errorf("field %q lists values, which only a TYPE_TEXT field may", name)
		}
	default:
		return errors.Errorf("field %q type must be TYPE_INTEGER, TYPE_NUMBER, TYPE_TEXT, TYPE_BOOL or TYPE_PLAYER", name)
	}
	if len(values) > valueLimit {
		return errors.Errorf("field %q lists more than %d values", name, valueLimit)
	}
	for i, value := range values {
		if value == "" || len(value) > TextLimit || !utf8.ValidString(value) {
			return errors.Errorf("field %q value %q must be 1 to %d bytes of text", name, value, TextLimit)
		}
		if slices.Contains(values[:i], value) {
			return errors.Errorf("field %q lists value %q twice", name, value)
		}
	}
	return nil
}

// Compatible checks that later keeps the type of every field earlier
// declares under the same kind, since a stored field would otherwise hold two
// kinds of value. A later release may add or drop kinds and fields.
func Compatible(earlier, later []*recordspb.RecordKind) error {
	for _, kind := range later {
		i := slices.IndexFunc(earlier, func(k *recordspb.RecordKind) bool { return k.GetName() == kind.GetName() })
		if i < 0 {
			continue
		}
		for _, field := range kind.GetFields() {
			for _, was := range earlier[i].GetFields() {
				if was.GetName() == field.GetName() && was.GetType() != field.GetType() {
					return errors.Errorf("record %q field %q was %s and cannot become %s; give the new field a new name", kind.GetName(), field.GetName(), was.GetType(), field.GetType())
				}
			}
		}
	}
	return nil
}

// Dropped names one field Check left out of a record, and why.
type Dropped struct {
	// Field is the field's name.
	Field string
	// Reason says why the field was left out.
	Reason string
}

// Check checks record against the kind manifest declares for it. An
// undeclared kind refuses the whole record. A field the kind does not
// declare, or whose value does not fit its declaration, is left out of the
// returned copy and named in dropped, so the rest of the record is kept.
func Check(manifest *wasm.Manifest, record *recordspb.GameRecord) (checked *recordspb.GameRecord, dropped []Dropped, err error) {
	// Find the record's kind.
	kind := Find(manifest, record.GetKind())
	if kind == nil {
		return nil, nil, errors.Errorf("mod.json declares no record %q", record.GetKind())
	}

	// Keep each field that fits its declaration, in name order so the
	// dropped list reads the same each time.
	checked = &recordspb.GameRecord{Kind: kind.GetName()}
	names := make([]string, 0, len(record.GetFields()))
	for name := range record.GetFields() {
		names = append(names, name)
	}
	slices.Sort(names)
	for _, name := range names {
		value := record.GetFields()[name]
		i := slices.IndexFunc(kind.GetFields(), func(f *recordspb.RecordField) bool { return f.GetName() == name })
		if i < 0 {
			dropped = append(dropped, Dropped{name, "not declared"})
			continue
		}
		if reason := fits(kind.GetFields()[i], value); reason != "" {
			dropped = append(dropped, Dropped{name, reason})
			continue
		}
		if checked.Fields == nil {
			checked.Fields = map[string]*recordspb.RecordValue{}
		}
		checked.Fields[name] = value
	}
	return checked, dropped, nil
}

// fits returns why value does not fit field, or empty when it fits.
func fits(field *recordspb.RecordField, value *recordspb.RecordValue) string {
	switch field.GetType() {
	case recordspb.RecordField_TYPE_INTEGER:
		if _, ok := value.GetValue().(*recordspb.RecordValue_Integer); !ok {
			return "not a whole number"
		}
	case recordspb.RecordField_TYPE_NUMBER:
		switch v := value.GetValue().(type) {
		case *recordspb.RecordValue_Integer:
		case *recordspb.RecordValue_Number:
			if math.IsNaN(v.Number) || math.IsInf(v.Number, 0) {
				return "not a finite number"
			}
		default:
			return "not a number"
		}
	case recordspb.RecordField_TYPE_TEXT:
		v, ok := value.GetValue().(*recordspb.RecordValue_Text)
		switch {
		case !ok:
			return "not text"
		case len(v.Text) > TextLimit || !utf8.ValidString(v.Text):
			return "longer than " + strconv.Itoa(TextLimit) + " bytes"
		case len(field.GetValues()) != 0 && !slices.Contains(field.GetValues(), v.Text):
			return strconv.Quote(v.Text) + " is not one of its values"
		}
	case recordspb.RecordField_TYPE_BOOL:
		if _, ok := value.GetValue().(*recordspb.RecordValue_Bool); !ok {
			return "not true or false"
		}
	case recordspb.RecordField_TYPE_PLAYER:
		switch value.GetValue().(type) {
		case *recordspb.RecordValue_Player, *recordspb.RecordValue_SteamId:
		default:
			return "not a player"
		}
	}
	return ""
}

// Describe writes a record in one line, its fields in name order, such as
// "round: area=docks round=3 winner=seekers".
func Describe(record *recordspb.GameRecord) string {
	names := make([]string, 0, len(record.GetFields()))
	for name := range record.GetFields() {
		names = append(names, name)
	}
	slices.Sort(names)
	parts := []string{record.GetKind() + ":"}
	for _, name := range names {
		parts = append(parts, name+"="+describeValue(record.GetFields()[name]))
	}
	return strings.Join(parts, " ")
}

// describeValue writes one value as Describe shows it.
func describeValue(value *recordspb.RecordValue) string {
	switch v := value.GetValue().(type) {
	case *recordspb.RecordValue_Integer:
		return strconv.FormatInt(v.Integer, 10)
	case *recordspb.RecordValue_Number:
		return strconv.FormatFloat(v.Number, 'g', -1, 64)
	case *recordspb.RecordValue_Text:
		return strconv.Quote(v.Text)
	case *recordspb.RecordValue_Bool:
		return strconv.FormatBool(v.Bool)
	case *recordspb.RecordValue_Player:
		return "player " + strconv.Itoa(int(v.Player))
	case *recordspb.RecordValue_SteamId:
		return "steam " + strconv.FormatUint(v.SteamId, 10)
	}
	return "?"
}

// DescribeDropped writes the dropped fields in one line, such as
// "seconds (not a number), team (not declared)".
func DescribeDropped(dropped []Dropped) string {
	parts := make([]string, 0, len(dropped))
	for _, d := range dropped {
		parts = append(parts, d.Field+" ("+d.Reason+")")
	}
	return strings.Join(parts, ", ")
}
