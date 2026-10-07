// Package entity reads and writes the fields of live entities, sends them
// inputs and creates them, through typed classes generated from the game's
// schema and entity dump by modlock-entitygen.
//
// Each server entity class is a type over CEntityInstance, such as
// CCitadelPlayerPawn. Address an entity by its handle, then get or set a field
// through the method named after it without its m_ prefix:
//
//	pawn, _ := player.Pawn()
//	hero := entity.CCitadelPlayerPawn{Handle: pawn.GetEntity()}
//	level, err := hero.NLevel().Get()
//
// A class has the methods of the fields and inputs it declares. Convert it to
// a base class to reach the base's, which costs nothing:
//
//	base := entity.CBaseEntity(hero)
//	health, err := base.IHealth().Get()
//	err = base.IHealth().Set(health + 100)
//	_, err = base.InputKill()
//
// Each designer name is a Designer named in camel case, whose Create spawns
// the entity with the spawn key values its key value struct sets:
//
//	lane := int32(2)
//	boss, err := entity.NpcTrooperBoss.Create(
//		&mod.EntityOptions{Position: &mod.Vector{Z: 128}},
//		&entity.CNPC_TrooperBossKeys{LaneNum: &lane},
//	)
//
// Each field finds itself by name, so a game update that moves the field
// needs no new build of the mod. The host refuses a handle whose entity is
// not of the class, so a mismatched class fails instead of touching another
// entity's memory. mod.EntityClass names the class a handle holds.
package entity

import "github.com/paralin/modlock/mod"

// Field names one schema field of one entity: the class that declares it,
// its name, and how the host reads it.
type Field struct {
	// Handle names the entity.
	Handle uint32
	// ClassName is the class that declares the field.
	ClassName string
	// Name is the field's schema name, such as m_iHealth.
	Name string
	// Type is how the host reads and writes the field.
	Type mod.FieldType
}

// read reads the field as the type the host sends for it.
func read[T bool | string | float64 | int64 | *mod.Vector](f Field) (T, error) {
	value, err := mod.ReadField(f.Handle, f.ClassName, f.Name, f.Type)
	typed, _ := value.(T)
	return typed, err
}

// write writes the field.
func (f Field) write(value any) error {
	return mod.WriteField(f.Handle, f.ClassName, f.Name, f.Type, value)
}

// Numeric lists the Go types of numeric fields.
type Numeric interface {
	int8 | int16 | int32 | uint8 | uint16 | uint32 | int64 | uint64 | float32 | float64
}

// Number is a numeric field.
type Number[T Numeric] struct{ Field }

// Get reads the field.
func (f Number[T]) Get() (T, error) {
	if f.wide() {
		value, err := read[int64](f.Field)
		return T(value), err
	}
	value, err := read[float64](f.Field)
	return T(value), err
}

// Set writes the field.
func (f Number[T]) Set(value T) error {
	if f.wide() {
		return f.write(int64(value))
	}
	return f.write(float64(value))
}

// wide reports whether the host sends the field as an int64 integer rather
// than a float64 number, which holds a 64-bit integer inexactly.
func (f Number[T]) wide() bool {
	return f.Type == mod.FieldTypeInt64 || f.Type == mod.FieldTypeUint64
}

// Bool is a boolean field.
type Bool struct{ Field }

// Get reads the field.
func (f Bool) Get() (bool, error) { return read[bool](f.Field) }

// Set writes the field.
func (f Bool) Set(value bool) error { return f.write(value) }

// Text is a string field, which the host can read but not write.
type Text struct{ Field }

// Get reads the field.
func (f Text) Get() (string, error) { return read[string](f.Field) }

// Vector is a position or direction field.
type Vector struct{ Field }

// Get reads the field.
func (f Vector) Get() (*mod.Vector, error) { return read[*mod.Vector](f.Field) }

// Set writes the field.
func (f Vector) Set(value *mod.Vector) error { return f.write(value) }

// Angles is a rotation field, which the host reads as a vector of pitch, yaw
// and roll.
type Angles struct{ Field }

// Get reads the field.
func (f Angles) Get() (*mod.Angles, error) {
	value, err := read[*mod.Vector](f.Field)
	return &mod.Angles{Pitch: value.GetX(), Yaw: value.GetY(), Roll: value.GetZ()}, err
}

// Set writes the field.
func (f Angles) Set(value *mod.Angles) error {
	return f.write(&mod.Vector{X: value.GetPitch(), Y: value.GetYaw(), Z: value.GetRoll()})
}

// Class is an entity class: CEntityInstance or a class defined over it.
type Class interface {
	~struct{ Handle uint32 }
}

// Handle is a field that names another entity, of class T.
type Handle[T Class] struct{ Field }

// Get reads the field and addresses the entity it names.
func (f Handle[T]) Get() (T, error) {
	value, err := read[float64](f.Field)
	return T(CEntityInstance{Handle: uint32(value)}), err
}

// Set points the field at value.
func (f Handle[T]) Set(value T) error {
	return f.write(float64(CEntityInstance(value).Handle))
}
