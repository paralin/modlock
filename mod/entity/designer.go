package entity

import (
	"reflect"

	"github.com/paralin/modlock/mod"
	"github.com/paralin/modlock/src/wasm/entities"
)

// Designer is a designer name a mod creates entities of. E is the class that
// implements it and K its key value struct.
type Designer[E Class, K any] struct {
	// Name is the designer name, such as npc_trooper_boss.
	Name string
	// Subclass is the game data entry Create gives an entity whose options
	// name none, or empty when the entity needs none to survive its spawn.
	Subclass string
}

// Create creates an entity of the designer name from options, adding the key
// values keys sets, whose nil fields leave their keys unset. A nil keys sets
// none, and nil options take every default.
func (d Designer[E, K]) Create(options *mod.EntityOptions, keys *K) (E, error) {
	// Pass every option through, as the mod set it, under the designer name.
	request := options.CloneVT()
	if request == nil {
		request = &mod.EntityOptions{}
	}
	request.DesignerName = d.Name

	// Give the designer's subclass to an entity whose options name none, and
	// add the typed key values after the mod's own.
	if request.Subclass == "" {
		request.Subclass = d.Subclass
	}
	request.KeyValues = append(request.KeyValues, keyValues(keys)...)
	handle, err := mod.CreateEntity(request)
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
func entityValue(value any) *entities.EntityValue {
	switch value := value.(type) {
	case *bool:
		return &entities.EntityValue{Value: &entities.EntityValue_Boolean{Boolean: *value}}
	case *int32:
		return &entities.EntityValue{Value: &entities.EntityValue_Integer{Integer: *value}}
	case *float32:
		return &entities.EntityValue{Value: &entities.EntityValue_Number{Number: *value}}
	case *string:
		return &entities.EntityValue{Value: &entities.EntityValue_Text{Text: *value}}
	case *uint32:
		return &entities.EntityValue{Value: &entities.EntityValue_Color{Color: *value}}
	case *mod.Vector:
		return &entities.EntityValue{Value: &entities.EntityValue_Vector{Vector: value}}
	case *mod.Angles:
		vector := &mod.Vector{X: value.GetPitch(), Y: value.GetYaw(), Z: value.GetRoll()}
		return &entities.EntityValue{Value: &entities.EntityValue_Vector{Vector: vector}}
	}
	panic("modlock-entitygen wrote a key value of an unknown type")
}
