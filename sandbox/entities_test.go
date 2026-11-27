package sandbox

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/paralin/modlock/source2/vpk"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/entities"
	"github.com/paralin/modlock/src/wasm/fields"
)

// TestEntities covers the sandbox map-entity index: a fixture, ReadField
// through the generic schema table, and a small compiled lump.
func TestEntities(t *testing.T) {
	s := newSandbox(Config{})
	idx, err := loadEntityFixture(filepath.Join("testdata", entityFixtureFile))
	if err != nil {
		t.Fatal(err)
	}
	s.entities = idx
	build := testBuild("test")

	// FindEntitiesByName matches the stored name exactly, as the game does.
	// Handles stay stable from firstMapEntity.
	goHandles := mustFindByName(t, s, build, "go")
	if len(goHandles) != 1 || goHandles[0] != firstMapEntity {
		t.Fatalf("FindEntitiesByName(go) = %v, want [%d]", goHandles, firstMapEntity)
	}
	if found := mustFindByName(t, s, build, "missing"); len(found) != 0 {
		t.Fatalf("FindEntitiesByName(missing) = %v, want none", found)
	}

	placement := mustPosition(t, s, build, goHandles[0])
	if placement.GetPosition().GetX() != 625 || placement.GetPosition().GetY() != -700 || placement.GetPosition().GetZ() != 0.1 {
		t.Fatalf("go position = %+v, want 625,-700,0.1", placement.GetPosition())
	}
	if name := mustName(t, s, build, goHandles[0]); name != "go" {
		t.Fatalf("EntityName = %q, want go", name)
	}
	if class := mustClass(t, s, build, goHandles[0]); class != "point_clientui_world_panel" {
		t.Fatalf("EntityClass designer = %q, want point_clientui_world_panel", class)
	}

	// ReadField maps m_flWidth and m_flHeight through the generic table,
	// and accepts the keyvalue name itself.
	if width := mustFloat(t, s, build, goHandles[0], "m_flWidth"); width != 200 {
		t.Fatalf("m_flWidth = %v, want 200", width)
	}
	if height := mustFloat(t, s, build, goHandles[0], "m_flHeight"); height != 200 {
		t.Fatalf("m_flHeight = %v, want 200", height)
	}
	if width := mustFloat(t, s, build, goHandles[0], "width"); width != 200 {
		t.Fatalf("width = %v, want 200", width)
	}

	// A stale handle and an unknown type answer as the host does.
	request, _ := (&entities.EntityRequest{Entity: firstMapEntity + 99}).MarshalVT()
	if reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "EntityName", Request: request}); reply.GetError() != "no live entity has that handle" {
		t.Fatalf("stale EntityName answered %q", reply.GetError())
	}
	field, _ := (&fields.ReadFieldRequest{Entity: goHandles[0], Field: "width"}).MarshalVT()
	if reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "ReadField", Request: field}); reply.GetError() != "the field type is unknown" {
		t.Fatalf("untyped ReadField answered %q", reply.GetError())
	}

	panels := mustFind(t, s, build, "point_clientui_world_panel")
	if len(panels) != 2 {
		t.Fatalf("FindEntities(point_clientui_world_panel) = %v, want 2", panels)
	}
	brown := mustFindByName(t, s, build, "brown1")
	if len(brown) != 1 {
		t.Fatalf("FindEntitiesByName(brown1) = %v, want 1", brown)
	}
	if place := mustPosition(t, s, build, brown[0]); place.GetPosition().GetX() != 500 || place.GetPosition().GetY() != -700 {
		t.Fatalf("brown1 position = %+v, want 500,-700", place.GetPosition())
	}

	// A compiled default_ents lump is indexed the same way when the
	// archive has no world.
	dir := t.TempDir()
	vpkPath := filepath.Join(dir, "maps", "hl_test.vpk")
	if err := writeTestMap(vpkPath, false); err != nil {
		t.Fatal(err)
	}
	if s.entities, err = loadMapArchive(vpkPath, "hl_test"); err != nil {
		t.Fatal(err)
	}
	// The lump stores [PR#]start, a prefab's name, which the game finds
	// only as start.
	handles := mustFindByName(t, s, build, "start")
	if len(handles) != 1 {
		t.Fatalf("lump FindEntitiesByName(start) = %v, want 1", handles)
	}
	if found := mustFindByName(t, s, build, "[PR#]start"); len(found) != 0 {
		t.Fatalf("lump FindEntitiesByName([PR#]start) = %v, want none", found)
	}
	if name := mustName(t, s, build, handles[0]); name != "start" {
		t.Fatalf("lump EntityName = %q, want start", name)
	}
	if width := mustFloat(t, s, build, handles[0], "m_flWidth"); width != 64 {
		t.Fatalf("lump m_flWidth = %v, want 64", width)
	}
}

// TestEntitiesSkipAPartialSource checks that a map archive whose world lists
// a lump it lacks contributes nothing, and the fixture beside it serves the
// session instead of a mix of both.
func TestEntitiesSkipAPartialSource(t *testing.T) {
	// Put a broken archive and the fixture in one mod.
	mod := t.TempDir()
	if err := writeTestMap(filepath.Join(mod, "maps", "hl_test.vpk"), true); err != nil {
		t.Fatal(err)
	}
	fixture, err := os.ReadFile(filepath.Join("testdata", entityFixtureFile))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(mod, DataDir), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(mod, DataDir, entityFixtureFile), fixture, 0o644); err != nil {
		t.Fatal(err)
	}

	// Load the session's entities.
	var logs []string
	s := newSandbox(Config{Map: "hl_test", Mods: []string{mod}, Events: func(event *control.HostEvent) {
		logs = append(logs, event.GetLog().GetText())
	}})
	s.loadMapEntities()
	build := testBuild("test")
	if found := mustFindByName(t, s, build, "start"); len(found) != 0 {
		t.Fatalf("the partial archive left start behind: %v", found)
	}
	if found := mustFindByName(t, s, build, "go"); len(found) != 1 || found[0] != firstMapEntity {
		t.Fatalf("FindEntitiesByName(go) = %v, want the fixture's first handle", found)
	}
	if len(logs) != 2 || !strings.Contains(logs[0], "missing.vents_c") {
		t.Fatalf("logs = %q, want the archive's failure and the fixture's load", logs)
	}
}

func mustFindByName(t *testing.T, s *Sandbox, build *Build, name string) []uint32 {
	t.Helper()
	request, _ := (&entities.FindEntitiesByNameRequest{Name: name}).MarshalVT()
	return mustEntities(t, s.answer(build, &serviceCalls{}, &wasm.Call{Method: "FindEntitiesByName", Request: request}))
}

func mustFind(t *testing.T, s *Sandbox, build *Build, class string) []uint32 {
	t.Helper()
	request, _ := (&entities.FindEntitiesRequest{ClassName: class}).MarshalVT()
	return mustEntities(t, s.answer(build, &serviceCalls{}, &wasm.Call{Method: "FindEntities", Request: request}))
}

func mustEntities(t *testing.T, reply *wasm.Reply) []uint32 {
	t.Helper()
	if err := reply.GetError(); err != "" {
		t.Fatal(err)
	}
	response := &entities.EntitiesResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	return response.GetEntities()
}

func mustPosition(t *testing.T, s *Sandbox, build *Build, handle uint32) *entities.EntityPlacement {
	t.Helper()
	request, _ := (&entities.EntityRequest{Entity: handle}).MarshalVT()
	reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "EntityPosition", Request: request})
	if err := reply.GetError(); err != "" {
		t.Fatal(err)
	}
	response := &entities.EntityPositionResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	if response.GetPlacement() == nil {
		t.Fatal("EntityPosition returned no placement")
	}
	return response.GetPlacement()
}

func mustName(t *testing.T, s *Sandbox, build *Build, handle uint32) string {
	t.Helper()
	request, _ := (&entities.EntityRequest{Entity: handle}).MarshalVT()
	reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "EntityName", Request: request})
	if err := reply.GetError(); err != "" {
		t.Fatal(err)
	}
	response := &entities.EntityNameResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	return response.GetName()
}

func mustClass(t *testing.T, s *Sandbox, build *Build, handle uint32) string {
	t.Helper()
	request, _ := (&entities.EntityRequest{Entity: handle}).MarshalVT()
	reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "EntityClass", Request: request})
	if err := reply.GetError(); err != "" {
		t.Fatal(err)
	}
	response := &entities.EntityClassResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	return response.GetName().GetDesignerName()
}

func mustFloat(t *testing.T, s *Sandbox, build *Build, handle uint32, field string) float64 {
	t.Helper()
	request, _ := (&fields.ReadFieldRequest{
		Entity:    handle,
		ClassName: "CPointClientUIWorldPanel",
		Field:     field,
		Type:      fields.FieldType_FIELD_TYPE_FLOAT32,
	}).MarshalVT()
	reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "ReadField", Request: request})
	if err := reply.GetError(); err != "" {
		t.Fatal(err)
	}
	response := &fields.FieldResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	if response.GetValue() == nil {
		t.Fatalf("ReadField(%s) returned no value", field)
	}
	return response.GetValue().GetNumber()
}

// writeTestMap writes a map archive at vpkPath holding default_ents with one
// entity, the prefab entity [PR#]start. With broken set, it also holds a world listing that lump and
// one the archive lacks.
func writeTestMap(vpkPath string, broken bool) error {
	// Compile the lump.
	if err := os.MkdirAll(filepath.Dir(vpkPath), 0o755); err != nil {
		return err
	}
	lump := map[string]any{
		"m_childLumps": []any{},
		"m_entityKeyValues": []any{
			map[string]any{
				"keyValues3Data": map[string]any{
					"values": map[string]any{
						"targetname": "[PR#]start",
						"classname":  "point_clientui_world_panel",
						"origin":     "10 20 30",
						"angles":     "0 90 0",
						"width":      64,
						"height":     32,
					},
				},
			},
		},
	}
	tree := fstest.MapFS{}
	if err := writeCompiled(tree, "maps/hl_test/entities/default_ents.vents_c", lump); err != nil {
		return err
	}
	if broken {
		world := map[string]any{"m_entityLumps": []any{
			"maps/hl_test/entities/default_ents.vents",
			"maps/hl_test/entities/missing.vents",
		}}
		if err := writeCompiled(tree, "maps/hl_test/world.vwrld_c", world); err != nil {
			return err
		}
	}

	// Pack the archive.
	f, err := os.Create(vpkPath)
	if err != nil {
		return err
	}
	defer f.Close()
	return vpk.Write(f, tree)
}

// writeCompiled stores value in tree as a compiled resource named name.
func writeCompiled(tree fstest.MapFS, name string, value map[string]any) error {
	encoded, err := kv3.Encode(value)
	if err != nil {
		return err
	}
	compiled, err := resource.Write(1, []resource.Block{{Tag: "DATA", Data: encoded}})
	if err != nil {
		return err
	}
	tree[name] = &fstest.MapFile{Data: compiled}
	return nil
}
