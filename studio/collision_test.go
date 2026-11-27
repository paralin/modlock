package studio

import (
	"encoding/binary"
	"math"
	"testing"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
)

// TestCollisionParts checks that each part uses only its own valid bind pose.
func TestCollisionParts(t *testing.T) {
	for _, test := range []struct {
		name  string
		poses []any
		want  [3]float64
	}{
		{"missing", []any{[]any{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}}, [3]float64{0, 0, -1}},
		{"truncated", []any{[]any{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}, []any{1, 0}}, [3]float64{0, 0, -1}},
		{"valid", []any{[]any{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}, []any{1, 0, 0, 40, 0, 1, 0, 50, 0, 0, 1, 60}}, [3]float64{40, 50, 59}},
	} {
		t.Run(test.name, func(t *testing.T) {
			// Encode two sphere parts through the resource decoder's real format.
			part := map[string]any{"m_rnShape": map[string]any{"m_spheres": []any{
				map[string]any{"m_Sphere": map[string]any{"m_vCenter": []any{0, 0, 0}, "m_flRadius": 1}},
			}}}
			data, err := kv3.Encode(map[string]any{"m_bindPose": test.poses, "m_parts": []any{part, part}})
			if err != nil {
				t.Fatal(err)
			}

			// Require both spheres and their complete tessellated geometry.
			mesh := collision([]resource.Block{{Tag: "PHYS", Data: data}})
			vertices := (capRings + 2) * capSegments
			if mesh == nil || mesh.GetShapes() != 2 || len(mesh.GetPositions()) != 2*vertices*12 || len(mesh.GetIndices()) != 2*(capRings+1)*capSegments*6*4 {
				t.Fatalf("incomplete collision geometry: %v", mesh)
			}

			// The first pole receives its part's translation; the next is independent.
			for part, want := range [][3]float64{{10, 20, 29}, test.want} {
				for axis, value := range want {
					at := part*vertices*12 + axis*4
					got := float64(math.Float32frombits(binary.LittleEndian.Uint32(mesh.GetPositions()[at:])))
					if math.Abs(got-value) > 1e-5 {
						t.Fatalf("part %d axis %d = %g, want %g", part, axis, got, value)
					}
				}
			}
		})
	}
}
