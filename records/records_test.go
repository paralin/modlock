package records

import (
	"math"
	"strings"
	"testing"

	"github.com/paralin/modlock/src/wasm"
	recordspb "github.com/paralin/modlock/src/wasm/records"
)

// round is a kind of record with one field of each type.
var round = &recordspb.RecordKind{Name: "round", Fields: []*recordspb.RecordField{
	{Name: "round", Type: recordspb.RecordField_TYPE_INTEGER},
	{Name: "seconds", Type: recordspb.RecordField_TYPE_NUMBER},
	{Name: "area", Type: recordspb.RecordField_TYPE_TEXT, Values: []string{"docks", "temple"}},
	{Name: "prop", Type: recordspb.RecordField_TYPE_TEXT},
	{Name: "won", Type: recordspb.RecordField_TYPE_BOOL},
	{Name: "seeker", Type: recordspb.RecordField_TYPE_PLAYER},
}}

// TestValidate checks which declarations a build accepts.
func TestValidate(t *testing.T) {
	// Accept a well-formed kind and refuse each kind of bad one.
	if err := Validate(&wasm.Manifest{Records: []*recordspb.RecordKind{round}}); err != nil {
		t.Fatalf("Validate(round) = %v", err)
	}
	field := func(f *recordspb.RecordField) *recordspb.RecordKind {
		return &recordspb.RecordKind{Name: "find", Fields: []*recordspb.RecordField{f}}
	}
	for name, kind := range map[string]*recordspb.RecordKind{
		"kind name":   {Name: "Round"},
		"field name":  field(&recordspb.RecordField{Name: "9lives", Type: recordspb.RecordField_TYPE_INTEGER}),
		"reserved":    field(&recordspb.RecordField{Name: "game", Type: recordspb.RecordField_TYPE_TEXT}),
		"type":        field(&recordspb.RecordField{Name: "x"}),
		"values":      field(&recordspb.RecordField{Name: "x", Type: recordspb.RecordField_TYPE_INTEGER, Values: []string{"a"}}),
		"value twice": field(&recordspb.RecordField{Name: "x", Type: recordspb.RecordField_TYPE_TEXT, Values: []string{"a", "a"}}),
		"field twice": {Name: "find", Fields: []*recordspb.RecordField{
			{Name: "x", Type: recordspb.RecordField_TYPE_BOOL}, {Name: "x", Type: recordspb.RecordField_TYPE_BOOL},
		}},
	} {
		if Validate(&wasm.Manifest{Records: []*recordspb.RecordKind{kind}}) == nil {
			t.Errorf("Validate accepted a bad %s", name)
		}
	}
	if Validate(&wasm.Manifest{Records: []*recordspb.RecordKind{round, round}}) == nil {
		t.Error("Validate accepted a kind declared twice")
	}
}

// TestCheck keeps the fields that fit and drops the rest.
func TestCheck(t *testing.T) {
	// Refuse an undeclared kind.
	manifest := &wasm.Manifest{Records: []*recordspb.RecordKind{round}}
	if _, _, err := Check(manifest, &recordspb.GameRecord{Kind: "find"}); err == nil {
		t.Fatal("Check took an undeclared kind")
	}

	// Keep each fitting field, an integer for a number among them, and drop
	// each field that does not fit.
	integer := func(v int64) *recordspb.RecordValue {
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Integer{Integer: v}}
	}
	number := func(v float64) *recordspb.RecordValue {
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Number{Number: v}}
	}
	text := func(v string) *recordspb.RecordValue {
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Text{Text: v}}
	}
	checked, dropped, err := Check(manifest, &recordspb.GameRecord{Kind: "round", Fields: map[string]*recordspb.RecordValue{
		"round":   number(2.5),
		"seconds": integer(184),
		"area":    text("market"),
		"prop":    text(strings.Repeat("x", TextLimit+1)),
		"won":     {Value: &recordspb.RecordValue_Bool{Bool: false}},
		"seeker":  {Value: &recordspb.RecordValue_Player{Player: 2}},
		"team":    text("amber"),
	}})
	if err != nil {
		t.Fatal(err)
	}
	if got, want := Describe(checked), "round: seconds=184 seeker=player 2 won=false"; got != want {
		t.Errorf("Describe = %q, want %q", got, want)
	}
	if got, want := DescribeDropped(dropped), `area ("market" is not one of its values), prop (longer than 64 bytes), round (not a whole number), team (not declared)`; got != want {
		t.Errorf("DescribeDropped = %q, want %q", got, want)
	}

	// Drop a number that is not finite.
	_, dropped, _ = Check(manifest, &recordspb.GameRecord{Kind: "round", Fields: map[string]*recordspb.RecordValue{"seconds": number(math.NaN())}})
	if len(dropped) != 1 {
		t.Errorf("Check kept a NaN: dropped %v", dropped)
	}
}

// TestCompatible refuses a field whose type a later release changes.
func TestCompatible(t *testing.T) {
	later := round.CloneVT()
	later.Fields = append(later.Fields, &recordspb.RecordField{Name: "found", Type: recordspb.RecordField_TYPE_INTEGER})
	if err := Compatible([]*recordspb.RecordKind{round}, []*recordspb.RecordKind{later}); err != nil {
		t.Fatalf("Compatible refused an added field: %v", err)
	}
	later.Fields[0].Type = recordspb.RecordField_TYPE_TEXT
	if Compatible([]*recordspb.RecordKind{round}, []*recordspb.RecordKind{later}) == nil {
		t.Fatal("Compatible accepted a changed type")
	}
}
