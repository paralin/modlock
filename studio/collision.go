package studio

import (
	"encoding/binary"
	"math"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
)

// Tessellation of spheres and capsules: rings from pole to pole and segments
// around the axis.
const (
	capRings    = 8
	capSegments = 12
)

// vec3 is a point or direction in model space.
type vec3 [3]float64

// add returns a + b.
func (a vec3) add(b vec3) vec3 { return vec3{a[0] + b[0], a[1] + b[1], a[2] + b[2]} }

// sub returns a - b.
func (a vec3) sub(b vec3) vec3 { return vec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]} }

// scale returns a times s.
func (a vec3) scale(s float64) vec3 { return vec3{a[0] * s, a[1] * s, a[2] * s} }

// length returns a's length.
func (a vec3) length() float64 { return math.Sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]) }

// cross returns a × b.
func (a vec3) cross(b vec3) vec3 {
	return vec3{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}

// numbers returns a decoded KeyValues3 list of numbers.
func numbers(value any) []float64 {
	var out []float64
	for _, item := range list(value) {
		out = append(out, number(item))
	}
	return out
}

// readVec3 returns a decoded KeyValues3 vector.
func readVec3(value any) vec3 {
	var out vec3
	copy(out[:], numbers(value))
	return out
}

// readPoints reads a blob of little-endian 32-bit floats, three to a point.
func readPoints(data []byte) []vec3 {
	out := make([]vec3, len(data)/12)
	for i := range out {
		for k := range 3 {
			out[i][k] = float64(math.Float32frombits(binary.LittleEndian.Uint32(data[i*12+k*4:])))
		}
	}
	return out
}

// collisionMesh gathers triangles in model space, each part placed by its
// bone's bind pose.
type collisionMesh struct {
	positions, indices []byte
	vertices           uint32
	shapes             uint32
	pose               []float64
}

// collision reads every collision shape of a model's PHYS block, or returns
// nil for a model without one. A shape it cannot read is left out.
func collision(blocks []resource.Block) *Collision {
	phys, err := decodeTag(blocks, "PHYS")
	if err != nil {
		return nil
	}
	poses := list(phys["m_bindPose"])
	m := &collisionMesh{}
	for index, part := range list(phys["m_parts"]) {
		part, _ := part.(map[string]any)
		shape, _ := part["m_rnShape"].(map[string]any)
		m.pose = nil
		if index < len(poses) {
			if pose := numbers(poses[index]); len(pose) == 12 {
				m.pose = pose
			}
		}
		for _, item := range list(shape["m_spheres"]) {
			sphere := field(item, "m_Sphere")
			center := readVec3(sphere["m_vCenter"])
			m.capsule(center, center, number(sphere["m_flRadius"]))
		}
		for _, item := range list(shape["m_capsules"]) {
			capsule := field(item, "m_Capsule")
			centers := list(capsule["m_vCenter"])
			if len(centers) == 2 {
				m.capsule(readVec3(centers[0]), readVec3(centers[1]), number(capsule["m_flRadius"]))
			}
		}
		for _, item := range list(shape["m_hulls"]) {
			m.hull(field(item, "m_Hull"))
		}
		for _, item := range list(shape["m_meshes"]) {
			m.mesh(field(item, "m_Mesh"))
		}
	}
	if m.shapes == 0 {
		return nil
	}
	return &Collision{Positions: m.positions, Indices: m.indices, Shapes: m.shapes}
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

// vertex adds a point, placed by the part's bind pose, and returns its
// index.
func (m *collisionMesh) vertex(p vec3) uint32 {
	if r := m.pose; r != nil {
		p = vec3{
			r[0]*p[0] + r[1]*p[1] + r[2]*p[2] + r[3],
			r[4]*p[0] + r[5]*p[1] + r[6]*p[2] + r[7],
			r[8]*p[0] + r[9]*p[1] + r[10]*p[2] + r[11],
		}
	}
	for _, c := range p {
		m.positions = binary.LittleEndian.AppendUint32(m.positions, math.Float32bits(float32(c)))
	}
	m.vertices++
	return m.vertices - 1
}

// triangle adds a triangle of three vertex indices.
func (m *collisionMesh) triangle(a, b, c uint32) {
	for _, i := range [3]uint32{a, b, c} {
		m.indices = binary.LittleEndian.AppendUint32(m.indices, i)
	}
}

// capsule adds a capsule between two centers, or a sphere when they meet,
// as rings of latitude: the lower half around c0 and the upper around c1.
func (m *collisionMesh) capsule(c0, c1 vec3, radius float64) {
	// Build a frame around the axis.
	axis := c1.sub(c0)
	if axis.length() < 1e-6 {
		axis = vec3{0, 0, 1}
	}
	axis = axis.scale(1 / axis.length())
	side := vec3{1, 0, 0}
	if math.Abs(axis[0]) > 0.9 {
		side = vec3{0, 1, 0}
	}
	u := axis.cross(side)
	u = u.scale(1 / u.length())
	v := axis.cross(u)

	// Add each ring; the equator appears once per half.
	first := m.vertices
	rings := 0
	for half, center := range [2]vec3{c0, c1} {
		for i := half * capRings / 2; i <= capRings/2+half*capRings/2; i++ {
			lat := math.Pi * (float64(i)/capRings - 0.5)
			for j := range capSegments {
				lon := 2 * math.Pi * float64(j) / capSegments
				dir := u.scale(math.Cos(lat) * math.Cos(lon)).add(v.scale(math.Cos(lat) * math.Sin(lon))).add(axis.scale(math.Sin(lat)))
				m.vertex(center.add(dir.scale(radius)))
			}
			rings++
		}
	}

	// Join each ring to the next.
	for ring := range rings - 1 {
		for j := range capSegments {
			a := first + uint32(ring*capSegments+j)
			b := first + uint32(ring*capSegments+(j+1)%capSegments)
			m.triangle(a, b, b+capSegments)
			m.triangle(a, b+capSegments, a+capSegments)
		}
	}
	m.shapes++
}

// hull adds a convex hull, fanning each face around the loop of half-edges
// that bounds it. Each edge is 4 bytes: the next edge around its face, its
// twin, its origin vertex and its face.
func (m *collisionMesh) hull(hull map[string]any) {
	positions, edges, faces := blob(hull["m_VertexPositions"]), blob(hull["m_Edges"]), blob(hull["m_Faces"])
	if len(positions) == 0 || len(edges) == 0 || len(faces) == 0 {
		return
	}
	first := m.vertices
	points := readPoints(positions)
	count := len(points)
	for _, p := range points {
		m.vertex(p)
	}
	for _, start := range faces {
		// Walk the face's loop, at most once around every edge.
		var loop []uint32
		for e, n := int(start), 0; n < len(edges)/4; n++ {
			if e*4+3 >= len(edges) || int(edges[e*4+2]) >= count {
				break
			}
			loop = append(loop, first+uint32(edges[e*4+2]))
			if e = int(edges[e*4]); e == int(start) {
				break
			}
		}
		for i := 2; i < len(loop); i++ {
			m.triangle(loop[0], loop[i-1], loop[i])
		}
	}
	m.shapes++
}

// mesh adds a triangle mesh: vertices as three floats and triangles as
// three 32-bit vertex indices.
func (m *collisionMesh) mesh(mesh map[string]any) {
	points, triangles := readPoints(blob(mesh["m_Vertices"])), blob(mesh["m_Triangles"])
	count := uint32(len(points))
	if count == 0 {
		return
	}
	first := m.vertices
	for _, p := range points {
		m.vertex(p)
	}
	for i := 0; i+12 <= len(triangles); i += 12 {
		a := binary.LittleEndian.Uint32(triangles[i:])
		b := binary.LittleEndian.Uint32(triangles[i+4:])
		c := binary.LittleEndian.Uint32(triangles[i+8:])
		if a < count && b < count && c < count {
			m.triangle(first+a, first+b, first+c)
		}
	}
	m.shapes++
}
