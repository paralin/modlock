package mod

import (
	"errors"
	"math"

	recordspb "github.com/paralin/modlock/src/wasm/records"
)

// recordsService names the host service that takes records.
const recordsService = "records"

// Fields maps a record's field names to their values: any integer type, a
// float32 or float64, a string, a bool or a Player. A nil value leaves the
// field out.
type Fields map[string]any

// ErrNoRecords reports that the host keeps no records, as an older server
// does; Record logs the record instead.
var ErrNoRecords = errors.New("records: this host keeps no records")

// Record adds one record of a kind mod.json declares, such as one per round.
// An integer is sent as an integer and a float as a number; a Player is sent
// as its slot, which the host stores as the player's SteamID, and a Player
// who has left as 0. The host leaves out a field the kind does not declare
// or whose value does not fit, and logs it. Record answers at once and the
// host delivers later. It returns ErrNoRecords, and logs the record, when no
// records service takes it.
func Record(kind string, fields Fields) error {
	// Encode each field with a value the wire carries.
	record := &recordspb.GameRecord{Kind: kind, Fields: map[string]*recordspb.RecordValue{}}
	for name, value := range fields {
		if encoded := recordValue(value); encoded != nil {
			record.Fields[name] = encoded
		}
	}

	// Hand the record to the host, or log it when no service takes it.
	data, err := record.MarshalVT()
	if err != nil {
		return err
	}
	if _, err := CallService(recordsService, "Add", data); err != nil {
		Log("record ", kind, ": ", fields, " (not stored: ", err, ")")
		return ErrNoRecords
	}
	return nil
}

// recordValue encodes one field's value in the case its type takes, or nil
// for a value no case carries.
func recordValue(value any) *recordspb.RecordValue {
	switch v := value.(type) {
	case int:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Integer{Integer: int64(v)}}
	case int8:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Integer{Integer: int64(v)}}
	case int16:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Integer{Integer: int64(v)}}
	case int32:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Integer{Integer: int64(v)}}
	case int64:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Integer{Integer: v}}
	case uint:
		return unsignedValue(uint64(v))
	case uint8:
		return unsignedValue(uint64(v))
	case uint16:
		return unsignedValue(uint64(v))
	case uint32:
		return unsignedValue(uint64(v))
	case uint64:
		return unsignedValue(v)
	case float32:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Number{Number: float64(v)}}
	case float64:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Number{Number: v}}
	case string:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Text{Text: v}}
	case bool:
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Bool{Bool: v}}
	case Player:
		if _, err := v.generation(); err != nil {
			return &recordspb.RecordValue{Value: &recordspb.RecordValue_SteamId{}}
		}
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Player{Player: v.Slot()}}
	}
	return nil
}

// unsignedValue encodes an unsigned integer, as a number past the integer
// range.
func unsignedValue(v uint64) *recordspb.RecordValue {
	if v > math.MaxInt64 {
		return &recordspb.RecordValue{Value: &recordspb.RecordValue_Number{Number: float64(v)}}
	}
	return &recordspb.RecordValue{Value: &recordspb.RecordValue_Integer{Integer: int64(v)}}
}
