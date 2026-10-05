package mod

import (
	"slices"

	"github.com/paralin/modlock/proto/modlock/spot"
)

// Spot is a base map and the objects players placed on it. Nothing is
// compiled: loading a spot spawns each object as a solid model on the map
// the world already runs.
type Spot struct {
	Map string
	// Objects are the placed objects in placement order.
	Objects []*SpotObject
}

// SpotObject is one solid object placed in a spot.
type SpotObject struct {
	// Model is the model's path, which the mode precached.
	Model    string
	Position *Vector
	Facing   *Angles
	// Scale multiplies the model's size.
	Scale float32
	// Bounce launches a hero who stands on the object upward at this speed,
	// in units per second; zero makes an ordinary object.
	Bounce float32
	// PlacedBy is the Steam ID of the player who placed the object, or zero.
	PlacedBy uint64
}

// EncodeSpot returns the spot document. Equal spots encode to equal bytes;
// positions and angles keep single precision.
func EncodeSpot(s *Spot) ([]byte, error) {
	doc := &spot.Spot{Map: s.Map}
	models := map[string]uint32{}
	builders := map[uint64]uint32{}
	for _, object := range s.Objects {
		model, ok := models[object.Model]
		if !ok {
			model = uint32(len(doc.Models))
			models[object.Model] = model
			doc.Models = append(doc.Models, object.Model)
		}
		var builder uint32
		if object.PlacedBy != 0 {
			if builder, ok = builders[object.PlacedBy]; !ok {
				doc.Builders = append(doc.Builders, object.PlacedBy)
				builder = uint32(len(doc.Builders))
				builders[object.PlacedBy] = builder
			}
		}
		scale := object.Scale
		if scale == 1 {
			scale = 0
		}
		doc.Objects = append(doc.Objects, &spot.SpotObject{
			Model:   model,
			X:       float32(object.Position.GetX()),
			Y:       float32(object.Position.GetY()),
			Z:       float32(object.Position.GetZ()),
			Pitch:   float32(object.Facing.GetPitch()),
			Yaw:     float32(object.Facing.GetYaw()),
			Roll:    float32(object.Facing.GetRoll()),
			Scale:   scale,
			Bounce:  object.Bounce,
			Builder: builder,
		})
	}
	return doc.MarshalVT()
}

// DecodeSpot reads a spot document.
func DecodeSpot(data []byte) (*Spot, error) {
	doc := &spot.Spot{}
	if err := doc.UnmarshalVT(data); err != nil {
		return nil, err
	}
	s := &Spot{Map: doc.GetMap()}
	for _, object := range doc.GetObjects() {
		decoded := &SpotObject{
			Position: &Vector{X: float64(object.GetX()), Y: float64(object.GetY()), Z: float64(object.GetZ())},
			Facing:   &Angles{Pitch: float64(object.GetPitch()), Yaw: float64(object.GetYaw()), Roll: float64(object.GetRoll())},
			Scale:    object.GetScale(),
			Bounce:   object.GetBounce(),
		}
		if decoded.Scale == 0 {
			decoded.Scale = 1
		}
		if index := int(object.GetModel()); index < len(doc.GetModels()) {
			decoded.Model = doc.GetModels()[index]
		}
		if index := int(object.GetBuilder()) - 1; index >= 0 && index < len(doc.GetBuilders()) {
			decoded.PlacedBy = doc.GetBuilders()[index]
		}
		s.Objects = append(s.Objects, decoded)
	}
	return s, nil
}

// Placed is one spot object standing in the world.
type Placed struct {
	Object *SpotObject
	World  WorldObject
	// Entity is the model's entity handle, as traces report it.
	Entity uint32
}

// feetProbe is how far below a hero's origin the bounce trace reaches.
const feetProbe = 8

// LoadedSpot is a spot standing in the world, in placement order.
type LoadedSpot struct {
	Map    string
	placed []*Placed
}

// LoadSpot places every object of s and returns it standing. Objects the
// game cannot place are skipped with a log line; the rest load.
func LoadSpot(s *Spot) *LoadedSpot {
	loaded := &LoadedSpot{Map: s.Map}
	for _, object := range s.Objects {
		_, _ = loaded.Place(object)
	}
	return loaded
}

// Objects returns the placed objects in placement order.
func (l *LoadedSpot) Objects() []*Placed {
	return l.placed
}

// Place spawns object as a solid model and adds it to the spot. A model the
// game cannot place logs why and returns the error.
func (l *LoadedSpot) Place(object *SpotObject) (*Placed, error) {
	scale := object.Scale
	if scale == 0 {
		scale = 1
	}
	solid := true
	world, err := CreateModel(&ModelOptions{
		Resource: object.Model,
		Position: object.Position,
		Facing:   object.Facing,
		Scale:    &scale,
		Solid:    &solid,
	})
	if err != nil {
		Log("spot: skipped ", object.Model, ": ", err)
		return nil, err
	}
	entity, err := world.Entity()
	if err != nil {
		_ = world.Remove()
		Log("spot: skipped ", object.Model, ": ", err)
		return nil, err
	}
	placed := &Placed{Object: object, World: world, Entity: entity}
	l.placed = append(l.placed, placed)
	return placed, nil
}

// At returns the placed object whose entity a trace hit, or nil.
func (l *LoadedSpot) At(entity uint32) *Placed {
	for _, placed := range l.placed {
		if placed.Entity == entity {
			return placed
		}
	}
	return nil
}

// Remove takes a placed object out of the world and the spot.
func (l *LoadedSpot) Remove(placed *Placed) {
	if index := slices.Index(l.placed, placed); index >= 0 {
		_ = placed.World.Remove()
		l.placed = slices.Delete(l.placed, index, index+1)
	}
}

// Clear removes every placed object.
func (l *LoadedSpot) Clear() {
	for _, placed := range l.placed {
		_ = placed.World.Remove()
	}
	l.placed = nil
}

// Spot returns the spot as it stands.
func (l *LoadedSpot) Spot() *Spot {
	objects := make([]*SpotObject, len(l.placed))
	for i, placed := range l.placed {
		objects[i] = placed.Object
	}
	return &Spot{Map: l.Map, Objects: objects}
}

// Bounce launches each sampled hero who stands on a bouncing object. A mode
// calls it with each frame's movement samples, for the heroes it watches.
func (l *LoadedSpot) Bounce(movement []*MovementSample) {
	if !slices.ContainsFunc(l.placed, func(p *Placed) bool { return p.Object.Bounce > 0 }) {
		return
	}
	for _, sample := range movement {
		velocity := sample.GetVelocity()
		if !sample.GetGrounded() || velocity.GetZ() > 0 {
			continue
		}
		feet := sample.GetPosition()
		hit, err := Trace(&TraceOptions{
			Start:  &Vector{X: feet.GetX(), Y: feet.GetY(), Z: feet.GetZ() + feetProbe},
			End:    &Vector{X: feet.GetX(), Y: feet.GetY(), Z: feet.GetZ() - feetProbe},
			Ignore: []uint32{sample.GetPawn()},
		})
		if err != nil || hit == nil {
			continue
		}
		if placed := l.At(hit.GetEntity()); placed != nil && placed.Object.Bounce > 0 {
			launch := &Vector{X: velocity.GetX(), Y: velocity.GetY(), Z: float64(placed.Object.Bounce)}
			_ = Player{Slot: sample.GetPlayer()}.SetVelocity(launch)
		}
	}
}
