// Package entity reads the fields of live entities through typed classes,
// one per server entity class, generated from the game's schema by
// modlock-entitygen. Address an entity by its handle and read its fields:
//
//	pawn, _ := player.Pawn()
//	health, err := entity.NewCCitadelPlayerPawn(pawn.GetEntity()).IHealth()
//
// Each getter reads its field by name, so a game update that moves the field
// needs no new build of the mod.
package entity

import "github.com/paralin/modlock/mod"

// read reads one field as the type the host sends for it.
func read[T bool | string | float64 | int64 | *mod.Vector](handle uint32, className, field string, kind mod.FieldType) (T, error) {
	var zero T
	value, err := mod.ReadField(handle, className, field, kind)
	if err != nil {
		return zero, err
	}
	typed, _ := value.(T)
	return typed, nil
}

// number reads one field the host sends as a float64 number.
func number[T int8 | int16 | int32 | uint8 | uint16 | uint32 | float32 | float64](handle uint32, className, field string, kind mod.FieldType) (T, error) {
	value, err := read[float64](handle, className, field, kind)
	return T(value), err
}

// integer reads one 64-bit field the host sends as an int64 integer.
func integer[T int64 | uint64](handle uint32, className, field string, kind mod.FieldType) (T, error) {
	value, err := read[int64](handle, className, field, kind)
	return T(value), err
}

// angles reads one rotation field.
func angles(handle uint32, className, field string) (*mod.Angles, error) {
	value, err := read[*mod.Vector](handle, className, field, mod.FieldTypeVector)
	return &mod.Angles{Pitch: value.GetX(), Yaw: value.GetY(), Roll: value.GetZ()}, err
}
