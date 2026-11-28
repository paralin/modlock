package physics

import (
	"testing"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
)

// TestShapes reads a part's own sphere and a compound's capsule, each with
// its attribute, as a compiled map packs its world into compounds.
func TestShapes(t *testing.T) {
	data, err := kv3.Encode(map[string]any{
		"m_collisionAttributes": []any{
			map[string]any{"m_CollisionGroupString": "Default"},
			map[string]any{
				"m_CollisionGroupString":   "ConditionallySolid",
				"m_InteractAsStrings":      []any{"passbullets"},
				"m_InteractExcludeStrings": []any{"Citadel_Bullet"},
			},
		},
		"m_parts": []any{map[string]any{"m_rnShape": map[string]any{
			"m_spheres": []any{map[string]any{
				"m_nCollisionAttributeIndex": 1,
				"m_Sphere":                   map[string]any{"m_vCenter": []any{0.0, 0.0, 0.0}, "m_flRadius": 10.0},
			}},
			"m_compounds": []any{map[string]any{
				"m_nCollisionAttributeIndex": 0,
				"m_Compound": map[string]any{"m_Capsules": []any{map[string]any{
					"m_flRadius": 5.0,
					"m_vCenter":  []any{[]any{0.0, 0.0, 0.0}, []any{0.0, 0.0, 100.0}},
				}}},
			}},
		}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	phys, err := Read([]resource.Block{{Tag: "PHYS", Data: data}})
	if err != nil {
		t.Fatal(err)
	}

	// The attributes keep their group and layers.
	attrs := phys.Attributes()
	if len(attrs) != 2 || attrs[0].Group != "Default" || attrs[1].Exclude[0] != "Citadel_Bullet" {
		t.Fatalf("attributes %+v", attrs)
	}

	// Each shape carries its own attribute, and the compound's capsule spans
	// its two centers.
	var got []Shape
	if n := phys.Shapes(func(s Shape) { got = append(got, s) }); n != 2 {
		t.Fatalf("%d shapes", n)
	}
	if got[0].Attribute != 1 || got[1].Attribute != 0 || !got[0].Solid || !got[1].Solid {
		t.Fatalf("attributes %d, %d", got[0].Attribute, got[1].Attribute)
	}
	top := 0.0
	for _, p := range got[1].Points {
		top = max(top, p[2])
	}
	if top < 104 || top > 106 {
		t.Fatalf("capsule top %v", top)
	}
}

// TestPartPoses checks that each part uses only its own valid bind pose.
func TestPartPoses(t *testing.T) {
	for _, test := range []struct {
		name  string
		poses []any
		want  Vec3
	}{
		{"missing", []any{[]any{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}}, Vec3{0, 0, -1}},
		{"truncated", []any{[]any{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}, []any{1, 0}}, Vec3{0, 0, -1}},
		{"valid", []any{[]any{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}, []any{1, 0, 0, 40, 0, 1, 0, 50, 0, 0, 1, 60}}, Vec3{40, 50, 59}},
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
			phys, err := Read([]resource.Block{{Tag: "PHYS", Data: data}})
			if err != nil {
				t.Fatal(err)
			}

			// Each sphere's first point is its lower pole, placed by its own
			// part's pose alone.
			var poles []Vec3
			phys.Shapes(func(s Shape) { poles = append(poles, s.Points[0]) })
			if len(poles) != 2 {
				t.Fatalf("%d shapes", len(poles))
			}
			for i, want := range []Vec3{{10, 20, 29}, test.want} {
				if poles[i].Sub(want).Length() > 1e-5 {
					t.Fatalf("part %d pole %v, want %v", i, poles[i], want)
				}
			}
		})
	}
}
