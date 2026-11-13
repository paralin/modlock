package mod

import (
	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// persistService names the host service that keeps players' records.
const persistService = "persist"

// PersistRecord is one player's record of a type the manifest declares under
// persist: each field written so far, by name.
type PersistRecord = persistpb.Record

// PersistOp is one change to one field of a record.
type PersistOp = persistpb.Op

// PersistAdd adds n to a count or number field; a count takes only a whole
// number from zero up.
func PersistAdd(field string, n float64) *PersistOp {
	return &PersistOp{Field: field, Op: &persistpb.Op_Add{Add: n}}
}

// PersistMax raises a number or max field to n when n is higher.
func PersistMax(field string, n float64) *PersistOp {
	return &PersistOp{Field: field, Op: &persistpb.Op_Max{Max: n}}
}

// PersistMin lowers a number field to n when n is lower.
func PersistMin(field string, n float64) *PersistOp {
	return &PersistOp{Field: field, Op: &persistpb.Op_Min{Min: n}}
}

// PersistSetNumber sets a count or number field to n.
func PersistSetNumber(field string, n float64) *PersistOp {
	return &PersistOp{Field: field, Op: &persistpb.Op_Set{Set: &persistpb.Value{Value: &persistpb.Value_Number{Number: n}}}}
}

// PersistSetText sets a string field to text.
func PersistSetText(field, text string) *PersistOp {
	return &PersistOp{Field: field, Op: &persistpb.Op_Set{Set: &persistpb.Value{Value: &persistpb.Value_Text{Text: text}}}}
}

// PersistInsert adds tag to a tags field.
func PersistInsert(field, tag string) *PersistOp {
	return &PersistOp{Field: field, Op: &persistpb.Op_Insert{Insert: tag}}
}

// PersistRemove takes tag from a tags field.
func PersistRemove(field, tag string) *PersistOp {
	return &PersistOp{Field: field, Op: &persistpb.Op_Remove{Remove: tag}}
}

// GetRecord reads the player's record of recordType over the host's persist
// service; done runs in a later frame with the record, empty for one never
// written, or the error that refused the read.
func (p Player) GetRecord(recordType string, done func(*PersistRecord, error)) {
	data, err := (&persistpb.GetRequest{Player: p.Slot, Type: recordType}).MarshalVT()
	if err != nil {
		done(nil, err)
		return
	}
	CallServiceLater(persistService, "Get", data, 0, func(reply []byte, err error) {
		if err != nil {
			done(nil, err)
			return
		}
		answer := &persistpb.GetReply{}
		if err := answer.UnmarshalVT(reply); err != nil {
			done(nil, err)
			return
		}
		done(recordOf(answer.GetRecord()), nil)
	})
}

// UpdateRecord changes the player's record of recordType by ops, which the
// host applies all or none, so two servers that update one record at once
// both land. With a key, such as a round's, an update the host already took
// is left out and done hears repeated. An update with a PersistAdd needs a
// key, since a resend without one would count twice; the host refuses it
// otherwise. done runs in a later frame and may be nil.
func (p Player) UpdateRecord(recordType string, ops []*PersistOp, key string, done func(record *PersistRecord, repeated bool, err error)) {
	if done == nil {
		done = func(*PersistRecord, bool, error) {}
	}
	data, err := (&persistpb.UpdateRequest{Player: p.Slot, Type: recordType, Ops: ops, UpdateKey: key}).MarshalVT()
	if err != nil {
		done(nil, false, err)
		return
	}
	CallServiceLater(persistService, "Update", data, 0, func(reply []byte, err error) {
		if err != nil {
			Log("persist ", recordType, " update: ", err)
			done(nil, false, err)
			return
		}
		answer := &persistpb.UpdateReply{}
		if err := answer.UnmarshalVT(reply); err != nil {
			done(nil, false, err)
			return
		}
		done(recordOf(answer.GetRecord()), answer.GetRepeated(), nil)
	})
}

// recordOf returns record, or an empty one.
func recordOf(record *PersistRecord) *PersistRecord {
	if record == nil {
		return &PersistRecord{}
	}
	return record
}
