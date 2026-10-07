package entity

import (
	"reflect"
	"slices"

	"github.com/paralin/modlock/mod"
	"github.com/paralin/modlock/proto/modlock/wasm"
)

// Designer is a designer name a mod creates entities of. E is the class that
// implements it and K its key value struct.
type Designer[E Class, K any] struct {
	// Name is the designer name, such as npc_trooper_boss.
	Name string
}

// Create creates an entity of the designer name from options, adding the key
// values keys sets, whose nil fields leave their keys unset. A nil keys sets
// none.
func (d Designer[E, K]) Create(options *mod.EntityOptions, keys *K) (E, error) {
	handle, err := mod.CreateEntity(&mod.EntityOptions{
		DesignerName: d.Name,
		Subclass:     options.GetSubclass(),
		Team:         options.GetTeam(),
		Position:     options.GetPosition(),
		Facing:       options.GetFacing(),
		KeyValues:    slices.Concat(options.GetKeyValues(), keyValues(keys)),
		Fields:       options.GetFields(),
	})
	return E(CEntityInstance{Handle: handle}), err
}

// keyValues returns the key values a key value struct sets.
func keyValues(keys any) []*mod.KeyValue {
	// A nil struct sets none.
	pointer := reflect.ValueOf(keys)
	if pointer.IsNil() {
		return nil
	}
	return appendKeyValues(nil, pointer.Elem())
}

// appendKeyValues appends the key values of each set field of keys, then
// those of the base key value struct it embeds.
func appendKeyValues(values []*mod.KeyValue, keys reflect.Value) []*mod.KeyValue {
	for i := range keys.NumField() {
		field, typ := keys.Field(i), keys.Type().Field(i)
		switch {
		case typ.Anonymous:
			values = appendKeyValues(values, field)
		case !field.IsNil():
			values = append(values, &mod.KeyValue{
				Key:   typ.Tag.Get("key"),
				Value: entityValue(field.Interface()),
			})
		}
	}
	return values
}

// entityValue encodes a key value field's value. An integer is an int32, a
// color a uint32 holding 0xRRGGBBAA, and angles travel as a vector of pitch,
// yaw and roll.
func entityValue(value any) *wasm.EntityValue {
	switch value := value.(type) {
	case *bool:
		return &wasm.EntityValue{Value: &wasm.EntityValue_Boolean{Boolean: *value}}
	case *int32:
		return &wasm.EntityValue{Value: &wasm.EntityValue_Integer{Integer: *value}}
	case *float32:
		return &wasm.EntityValue{Value: &wasm.EntityValue_Number{Number: *value}}
	case *string:
		return &wasm.EntityValue{Value: &wasm.EntityValue_Text{Text: *value}}
	case *uint32:
		return &wasm.EntityValue{Value: &wasm.EntityValue_Color{Color: *value}}
	case *mod.Vector:
		return &wasm.EntityValue{Value: &wasm.EntityValue_Vector{Vector: value}}
	case *mod.Angles:
		vector := &mod.Vector{X: value.GetPitch(), Y: value.GetYaw(), Z: value.GetRoll()}
		return &wasm.EntityValue{Value: &wasm.EntityValue_Vector{Vector: vector}}
	}
	panic("modlock-entitygen wrote a key value of an unknown type")
}
