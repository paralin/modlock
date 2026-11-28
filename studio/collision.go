package studio

import (
	"encoding/binary"
	"math"

	"github.com/paralin/modlock/source2/physics"
	"github.com/paralin/modlock/source2/resource"
)

// collision draws every collision shape of a model's PHYS block, compounds
// included, as one triangle mesh, or returns nil for a model without one.
func collision(blocks []resource.Block) *Collision {
	phys, err := physics.Read(blocks)
	if err != nil {
		return nil
	}
	out := &Collision{}
	var vertices uint32
	out.Shapes = uint32(phys.Shapes(func(shape physics.Shape) {
		for _, p := range shape.Points {
			for _, c := range p {
				out.Positions = binary.LittleEndian.AppendUint32(out.Positions, math.Float32bits(float32(c)))
			}
		}
		for _, t := range shape.Triangles {
			for _, i := range t {
				out.Indices = binary.LittleEndian.AppendUint32(out.Indices, vertices+i)
			}
		}
		vertices += uint32(len(shape.Points))
	}))
	if out.Shapes == 0 {
		return nil
	}
	return out
}
