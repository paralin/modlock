// Package physics reads the collision shapes of a Source 2 model's PHYS
// block, a map's world_physics model included, as triangles in model space
// with the collision attribute that says what each shape blocks.
//
// A model's physics is a list of parts, each with spheres, capsules, convex
// hulls and triangle meshes. A compiled map packs most of its world into
// compound shapes, each a bundle of hulls, meshes and capsules under one
// attribute. Shapes streams them all, one shape at a time, so a map's tens of
// thousands of hulls never sit in memory at once.
package physics

import (
	"encoding/binary"
	"math"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/pkg/errors"
)

// Tessellation of spheres and capsules: rings from pole to pole and segments
// around the axis.
const (
	capRings    = 8
	capSegments = 12
)

// Vec3 is a point or direction in model space.
type Vec3 [3]float64

// Add returns a + b.
func (a Vec3) Add(b Vec3) Vec3 { return Vec3{a[0] + b[0], a[1] + b[1], a[2] + b[2]} }

// Sub returns a - b.
func (a Vec3) Sub(b Vec3) Vec3 { return Vec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]} }

// Scale returns a times s.
func (a Vec3) Scale(s float64) Vec3 { return Vec3{a[0] * s, a[1] * s, a[2] * s} }

// Dot returns a · b.
func (a Vec3) Dot(b Vec3) float64 { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] }

// Length returns a's length.
func (a Vec3) Length() float64 { return math.Sqrt(a.Dot(a)) }

// Cross returns a × b.
func (a Vec3) Cross(b Vec3) Vec3 {
	return Vec3{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}

// Attribute is a collision attribute: the collision group a shape belongs to
// and the interaction layers it is, collides with, and ignores, as the game
// names them, such as "solid", "blocklos" or "Citadel_Bullet".
type Attribute struct {
	Group        string
	InteractAs   []string
	InteractWith []string
	Exclude      []string
}

// Shape is one collision shape as triangles: its points, placed by its
// part's bind pose, and triangles as three indices into Points. Attribute
// indexes Physics.Attributes, or is -1 when the shape names none.
type Shape struct {
	Attribute int
	Points    []Vec3
	Triangles [][3]uint32
}

// Physics is a decoded PHYS block.
type Physics struct {
	data map[string]any
}

// Read decodes the PHYS block of a compiled model's blocks.
func Read(blocks []resource.Block) (*Physics, error) {
	for _, block := range blocks {
		if block.Tag != "PHYS" {
			continue
		}
		value, err := kv3.Decode(block.Data)
		if err != nil {
			return nil, errors.Wrap(err, "PHYS")
		}
		data, _ := value.(map[string]any)
		return &Physics{data: data}, nil
	}
	return nil, errors.New("no PHYS block")
}

// Attributes returns the collision attributes shapes refer to.
func (p *Physics) Attributes() []Attribute {
	var out []Attribute
	for _, item := range list(p.data["m_collisionAttributes"]) {
		attr, _ := item.(map[string]any)
		group, _ := attr["m_CollisionGroupString"].(string)
		out = append(out, Attribute{
			Group:        group,
			InteractAs:   texts(attr["m_InteractAsStrings"]),
			InteractWith: texts(attr["m_InteractWithStrings"]),
			Exclude:      texts(attr["m_InteractExcludeStrings"]),
		})
	}
	return out
}

// Shapes calls fn with each shape of every part, compounds included, and
// returns how many it passed. A shape it cannot read is left out. fn may
// keep the shape; Shapes builds a new one for each call.
func (p *Physics) Shapes(fn func(Shape)) int {
	poses := list(p.data["m_bindPose"])
	count := 0
	emit := func(s *Shape) {
		if len(s.Triangles) != 0 {
			fn(*s)
			count++
		}
	}
	for index, part := range list(p.data["m_parts"]) {
		part, _ := part.(map[string]any)
		shape, _ := part["m_rnShape"].(map[string]any)
		var pose []float64
		if index < len(poses) {
			if values := numbers(poses[index]); len(values) == 12 {
				pose = values
			}
		}

		// The part's own shapes, each wrapping its geometry in a named field.
		for _, item := range list(shape["m_spheres"]) {
			sphere := field(item, "m_Sphere")
			center := readVec3(sphere["m_vCenter"])
			emit(capsule(attribute(item), pose, center, center, number(sphere["m_flRadius"])))
		}
		for _, item := range list(shape["m_capsules"]) {
			emit(capsuleOf(attribute(item), pose, field(item, "m_Capsule")))
		}
		for _, item := range list(shape["m_hulls"]) {
			emit(hull(attribute(item), pose, field(item, "m_Hull")))
		}
		for _, item := range list(shape["m_meshes"]) {
			emit(mesh(attribute(item), pose, field(item, "m_Mesh")))
		}

		// Each compound's shapes share the compound's attribute and hold their
		// geometry directly.
		for _, item := range list(shape["m_compounds"]) {
			attr := attribute(item)
			compound := field(item, "m_Compound")
			for _, sphere := range list(compound["m_Spheres"]) {
				sphere, _ := sphere.(map[string]any)
				center := readVec3(sphere["m_vCenter"])
				emit(capsule(attr, pose, center, center, number(sphere["m_flRadius"])))
			}
			for _, c := range list(compound["m_Capsules"]) {
				c, _ := c.(map[string]any)
				emit(capsuleOf(attr, pose, c))
			}
			for _, h := range list(compound["m_Hulls"]) {
				h, _ := h.(map[string]any)
				emit(hull(attr, pose, h))
			}
			for _, m := range list(compound["m_Meshes"]) {
				m, _ := m.(map[string]any)
				emit(mesh(attr, pose, m))
			}
		}
	}
	return count
}

// attribute returns a shape's collision attribute index, or -1.
func attribute(item any) int {
	object, _ := item.(map[string]any)
	if value, ok := object["m_nCollisionAttributeIndex"]; ok {
		return int(number(value))
	}
	return -1
}

// builder collects one shape, placing its points by the pose.
type builder struct {
	shape Shape
	pose  []float64
}

// point adds a point, placed by the pose, and returns its index.
func (b *builder) point(p Vec3) uint32 {
	if r := b.pose; r != nil {
		p = Vec3{
			r[0]*p[0] + r[1]*p[1] + r[2]*p[2] + r[3],
			r[4]*p[0] + r[5]*p[1] + r[6]*p[2] + r[7],
			r[8]*p[0] + r[9]*p[1] + r[10]*p[2] + r[11],
		}
	}
	b.shape.Points = append(b.shape.Points, p)
	return uint32(len(b.shape.Points) - 1)
}

// capsuleOf reads a capsule's two centers and radius.
func capsuleOf(attr int, pose []float64, c map[string]any) *Shape {
	centers := list(c["m_vCenter"])
	if len(centers) != 2 {
		return &Shape{}
	}
	return capsule(attr, pose, readVec3(centers[0]), readVec3(centers[1]), number(c["m_flRadius"]))
}

// capsule tessellates a capsule between two centers, or a sphere when they
// meet, as rings of latitude: the lower half around c0 and the upper around
// c1.
func capsule(attr int, pose []float64, c0, c1 Vec3, radius float64) *Shape {
	b := &builder{shape: Shape{Attribute: attr}, pose: pose}

	// Build a frame around the axis.
	axis := c1.Sub(c0)
	if axis.Length() < 1e-6 {
		axis = Vec3{0, 0, 1}
	}
	axis = axis.Scale(1 / axis.Length())
	side := Vec3{1, 0, 0}
	if math.Abs(axis[0]) > 0.9 {
		side = Vec3{0, 1, 0}
	}
	u := axis.Cross(side)
	u = u.Scale(1 / u.Length())
	v := axis.Cross(u)

	// Add each ring; the equator appears once per half.
	rings := 0
	for half, center := range [2]Vec3{c0, c1} {
		for i := half * capRings / 2; i <= capRings/2+half*capRings/2; i++ {
			lat := math.Pi * (float64(i)/capRings - 0.5)
			for j := range capSegments {
				lon := 2 * math.Pi * float64(j) / capSegments
				dir := u.Scale(math.Cos(lat) * math.Cos(lon)).Add(v.Scale(math.Cos(lat) * math.Sin(lon))).Add(axis.Scale(math.Sin(lat)))
				b.point(center.Add(dir.Scale(radius)))
			}
			rings++
		}
	}

	// Join each ring to the next.
	for ring := range rings - 1 {
		for j := range capSegments {
			a := uint32(ring*capSegments + j)
			n := uint32(ring*capSegments + (j+1)%capSegments)
			b.shape.Triangles = append(b.shape.Triangles,
				[3]uint32{a, n, n + capSegments},
				[3]uint32{a, n + capSegments, a + capSegments})
		}
	}
	return &b.shape
}

// hull triangulates a convex hull, fanning each face around the loop of
// half-edges that bounds it. Each edge is 4 bytes: the next edge around its
// face, its twin, its origin vertex and its face.
func hull(attr int, pose []float64, h map[string]any) *Shape {
	b := &builder{shape: Shape{Attribute: attr}, pose: pose}
	positions, edges, faces := blob(h["m_VertexPositions"]), blob(h["m_Edges"]), blob(h["m_Faces"])
	if len(positions) == 0 || len(edges) == 0 || len(faces) == 0 {
		return &b.shape
	}
	points := readPoints(positions)
	for _, p := range points {
		b.point(p)
	}
	for _, start := range faces {
		// Walk the face's loop, at most once around every edge.
		var loop []uint32
		for e, n := int(start), 0; n < len(edges)/4; n++ {
			if e*4+3 >= len(edges) || int(edges[e*4+2]) >= len(points) {
				break
			}
			loop = append(loop, uint32(edges[e*4+2]))
			if e = int(edges[e*4]); e == int(start) {
				break
			}
		}
		for i := 2; i < len(loop); i++ {
			b.shape.Triangles = append(b.shape.Triangles, [3]uint32{loop[0], loop[i-1], loop[i]})
		}
	}
	return &b.shape
}

// mesh reads a triangle mesh: vertices as three floats and triangles as
// three 32-bit vertex indices.
func mesh(attr int, pose []float64, m map[string]any) *Shape {
	b := &builder{shape: Shape{Attribute: attr}, pose: pose}
	points, triangles := readPoints(blob(m["m_Vertices"])), blob(m["m_Triangles"])
	count := uint32(len(points))
	for _, p := range points {
		b.point(p)
	}
	for i := 0; i+12 <= len(triangles); i += 12 {
		t := [3]uint32{
			binary.LittleEndian.Uint32(triangles[i:]),
			binary.LittleEndian.Uint32(triangles[i+4:]),
			binary.LittleEndian.Uint32(triangles[i+8:]),
		}
		if t[0] < count && t[1] < count && t[2] < count {
			b.shape.Triangles = append(b.shape.Triangles, t)
		}
	}
	return &b.shape
}

// readPoints reads points stored as three little-endian floats each.
func readPoints(data []byte) []Vec3 {
	out := make([]Vec3, len(data)/12)
	for i := range out {
		for k := range 3 {
			out[i][k] = float64(math.Float32frombits(binary.LittleEndian.Uint32(data[i*12+k*4:])))
		}
	}
	return out
}

// readVec3 returns a decoded KeyValues3 vector.
func readVec3(value any) Vec3 {
	var out Vec3
	copy(out[:], numbers(value))
	return out
}

// numbers returns a decoded KeyValues3 list of numbers.
func numbers(value any) []float64 {
	var out []float64
	for _, item := range list(value) {
		out = append(out, number(item))
	}
	return out
}

// texts returns a decoded KeyValues3 list of strings.
func texts(value any) []string {
	var out []string
	for _, item := range list(value) {
		if s, ok := item.(string); ok {
			out = append(out, s)
		}
	}
	return out
}

// field returns an object's field as an object, or nil.
func field(value any, key string) map[string]any {
	object, _ := value.(map[string]any)
	inner, _ := object[key].(map[string]any)
	return inner
}

// blob returns a decoded blob's bytes, or nil.
func blob(value any) []byte {
	b, _ := value.(kv3.Blob)
	return b.Data
}

// list returns value as a list, or nil.
func list(value any) []any {
	items, _ := value.([]any)
	return items
}

// number returns a decoded KeyValues3 number as a float.
func number(value any) float64 {
	switch v := value.(type) {
	case int64:
		return float64(v)
	case uint64:
		return float64(v)
	case float64:
		return v
	}
	return 0
}
