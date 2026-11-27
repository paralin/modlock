package sandbox

// Map entities are loaded so FindEntitiesByName, FindEntities, EntityPosition,
// EntityName, EntityClass and ReadField work without the game.
//
// The sandbox first reads maps/<map>.vpk from each built mod, and when
// Config.Fixtures is set, from the project beside that services directory. It
// opens the entity lumps named in maps/<map>/world.vwrld_c, or
// maps/<map>/entities/default_ents.vents_c, with the VPK, resource and kv3
// packages. A leading Hammer prefab prefix of the form [PR#] is stripped from
// targetnames so the names mods look up match the in-game names.
//
// When no lump can be read, it falls back to data/sandbox-entities.json in
// each built mod's data directory, or in the project as
// data/sandbox-entities.json. The file looks like:
//
//	{
//	  "entities": [
//	    {
//	      "targetname": "go",
//	      "classname": "point_clientui_world_panel",
//	      "origin": [0, 0, 0],
//	      "angles": [0, 90, 0],
//	      "keyvalues": {"width": 200, "height": 200}
//	    }
//	  ]
//	}

import (
	"encoding/json"
	"os"
	"path"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/paralin/modlock/source2/vpk"
	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/entities"
	"github.com/paralin/modlock/src/wasm/fields"
)

// firstMapEntity is the first handle assigned to a map entity, chosen so it
// cannot collide with stand-in pawn handles, which start at firstEntity.
const firstMapEntity = 0x01000000

// entityFixtureFile is the project fixture used when the map's entity lump
// cannot be read.
const entityFixtureFile = "sandbox-entities.json"

// schemaKeyValues maps a schema field name, as ReadField receives it, to the
// map keyvalue that stores that value. The table is generic Source 2 names,
// not a mode's own keys. An exact keyvalue name also matches.
var schemaKeyValues = map[string]string{
	"m_flWidth":             "width",
	"m_flHeight":            "height",
	"m_nRenderMode":         "rendermode",
	"m_iHealth":             "health",
	"m_iMaxHealth":          "max_health",
	"m_iTeamNum":            "teamnumber",
	"m_iName":               "targetname",
	"m_target":              "target",
	"m_iszModelName":        "model",
	"m_ModelName":           "model",
	"m_nSkin":               "skin",
	"m_nBody":               "body",
	"m_flGravityScale":      "gravity",
	"m_flDPI":               "panel_dpi",
	"m_flPanelDPI":          "panel_dpi",
	"m_bIgnoreInput":        "ignore_input",
	"m_flInteractDistance":  "interact_distance",
	"m_bLit":                "lit",
	"m_DialogXMLName":       "dialog_layout_name",
	"m_iszLayoutFile":       "dialog_layout_name",
	"m_iszDialogLayoutName": "dialog_layout_name",
	"m_bUseLocalOffset":     "useLocalOffset",
	"m_spawnflags":          "spawnflags",
}

// mapEntity is one loaded map entity.
type mapEntity struct {
	handle   uint32
	name     string
	rawName  string
	class    string
	position *geometry.Vec3
	facing   *geometry.EulerAngles
	keys     map[string]any
}

// entityIndex holds the session map's entities, addressed by handle, name or
// designer class.
type entityIndex struct {
	byHandle map[uint32]*mapEntity
	byName   map[string][]uint32
	byClass  map[string][]uint32
	order    []*mapEntity
}

func newEntityIndex() *entityIndex {
	return &entityIndex{
		byHandle: map[uint32]*mapEntity{},
		byName:   map[string][]uint32{},
		byClass:  map[string][]uint32{},
	}
}

// add records entity under the next stable handle.
func (idx *entityIndex) add(entity *mapEntity) {
	entity.handle = firstMapEntity + uint32(len(idx.order))
	idx.order = append(idx.order, entity)
	idx.byHandle[entity.handle] = entity
	if entity.class != "" {
		idx.byClass[entity.class] = append(idx.byClass[entity.class], entity.handle)
	}
	for _, name := range []string{entity.name, entity.rawName} {
		if name == "" {
			continue
		}
		if !containsHandle(idx.byName[name], entity.handle) {
			idx.byName[name] = append(idx.byName[name], entity.handle)
		}
	}
}

func containsHandle(handles []uint32, handle uint32) bool {
	for _, existing := range handles {
		if existing == handle {
			return true
		}
	}
	return false
}

// loadMapEntities fills the session's map entity index from the built mods'
// maps/<map>.vpk, or from data/sandbox-entities.json when no lump can be
// read. The 60 Hz frame clock is left unchanged.
func (s *Sandbox) loadMapEntities() {
	s.entities = newEntityIndex()
	if s.config.Map == "" {
		return
	}
	var sources []string
	for _, mod := range s.config.Mods {
		sources = append(sources, filepath.Join(mod, "maps", s.config.Map+".vpk"))
		sources = append(sources, filepath.Join(mod, "data", entityFixtureFile))
		if s.config.Fixtures != nil {
			if services := s.config.Fixtures(mod); services != "" {
				project := filepath.Dir(services)
				sources = append(sources, filepath.Join(project, "maps", s.config.Map+".vpk"))
				sources = append(sources, filepath.Join(project, DataDir, entityFixtureFile))
			}
		}
	}
	for _, source := range sources {
		var err error
		switch {
		case strings.EqualFold(filepath.Ext(source), ".vpk"):
			err = s.entities.loadVPK(source, s.config.Map)
		default:
			err = s.entities.loadFixtureFile(source)
		}
		if err != nil {
			if !os.IsNotExist(err) {
				s.log("", "map entities: "+source+": "+err.Error())
			}
			continue
		}
		if len(s.entities.order) == 0 {
			continue
		}
		s.log("", "map entities: loaded "+strconv.Itoa(len(s.entities.order))+" from "+source)
		return
	}
}

// loadVPK reads the entity lumps of the map archive at path.
func (idx *entityIndex) loadVPK(archivePath, mapName string) error {
	archive, err := vpk.OpenArchive(archivePath)
	if err != nil {
		return err
	}
	lumps := entityLumpNames(archive, mapName)
	for _, lump := range lumps {
		if err := idx.loadLump(archive, lump, 0); err != nil {
			return err
		}
	}
	if len(idx.order) == 0 {
		return os.ErrNotExist
	}
	return nil
}

// entityLumpNames returns the compiled entity lumps of a map archive: those
// the world resource lists, or default_ents when it lists none.
func entityLumpNames(archive *vpk.Archive, mapName string) []string {
	world := path.Join("maps", mapName, "world.vwrld_c")
	if data, err := readResourceObject(archive, world); err == nil {
		var names []string
		for _, item := range asList(data["m_entityLumps"]) {
			if name, _ := item.(string); name != "" {
				names = append(names, compiledLump(name))
			}
		}
		if len(names) > 0 {
			return names
		}
	}
	return []string{path.Join("maps", mapName, "entities", "default_ents.vents_c")}
}

// compiledLump is the compiled resource name of an entity lump listed by a
// world, which is stored without the trailing _c.
func compiledLump(name string) string {
	if strings.HasSuffix(name, "_c") {
		return name
	}
	return name + "_c"
}

// loadLump adds the entities of one compiled lump and its children.
func (idx *entityIndex) loadLump(archive *vpk.Archive, name string, depth int) error {
	if depth > 8 {
		return nil
	}
	data, err := readResourceObject(archive, name)
	if err != nil {
		return err
	}
	for _, item := range asList(data["m_entityKeyValues"]) {
		entity, _ := item.(map[string]any)
		kv3data, _ := entity["keyValues3Data"].(map[string]any)
		values, _ := kv3data["values"].(map[string]any)
		if values == nil {
			continue
		}
		idx.add(entityFromValues(values))
	}
	for _, item := range asList(data["m_childLumps"]) {
		child, _ := item.(string)
		if child == "" {
			continue
		}
		if err := idx.loadLump(archive, compiledLump(child), depth+1); err != nil {
			return err
		}
	}
	return nil
}

// readResourceObject decodes the DATA KeyValues3 block of a compiled resource
// in archive.
func readResourceObject(archive *vpk.Archive, name string) (map[string]any, error) {
	raw, err := archive.ReadFile(name)
	if err != nil {
		return nil, err
	}
	blocks, err := resource.Read(raw)
	if err != nil {
		return nil, err
	}
	data, ok := resource.Find(blocks, "DATA")
	if !ok {
		return nil, os.ErrNotExist
	}
	value, err := kv3.Decode(data)
	if err != nil {
		return nil, err
	}
	object, _ := value.(map[string]any)
	return object, nil
}

func (idx *entityIndex) loadFixtureFile(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	return idx.loadFixture(data)
}

// entityFixtureFileFormat is the JSON of data/sandbox-entities.json.
type entityFixtureFileFormat struct {
	Entities []entityFixture `json:"entities"`
}

// entityFixture is one entity in a project fixture.
type entityFixture struct {
	TargetName string         `json:"targetname"`
	ClassName  string         `json:"classname"`
	Origin     any            `json:"origin"`
	Angles     any            `json:"angles"`
	KeyValues  map[string]any `json:"keyvalues"`
}

func (idx *entityIndex) loadFixture(data []byte) error {
	var file entityFixtureFileFormat
	if err := json.Unmarshal(data, &file); err != nil {
		return err
	}
	if len(file.Entities) == 0 {
		return os.ErrNotExist
	}
	for _, item := range file.Entities {
		values := map[string]any{}
		for key, value := range item.KeyValues {
			values[key] = value
		}
		if item.TargetName != "" {
			values["targetname"] = item.TargetName
		}
		if item.ClassName != "" {
			values["classname"] = item.ClassName
		}
		if item.Origin != nil {
			values["origin"] = item.Origin
		}
		if item.Angles != nil {
			values["angles"] = item.Angles
		}
		idx.add(entityFromValues(values))
	}
	return nil
}

func entityFromValues(values map[string]any) *mapEntity {
	rawName := stringKey(values, "targetname")
	return &mapEntity{
		name:     stripPrefabPrefix(rawName),
		rawName:  rawName,
		class:    stringKey(values, "classname"),
		position: vec3Key(values["origin"]),
		facing:   anglesKey(values["angles"]),
		keys:     values,
	}
}

// stripPrefabPrefix removes a leading Hammer prefab marker such as [PR#],
// which compiled lumps store on targetnames and the game does not present to
// FindEntitiesByName.
func stripPrefabPrefix(name string) string {
	if strings.HasPrefix(name, "[PR") {
		if end := strings.IndexByte(name, ']'); end >= 0 {
			return name[end+1:]
		}
	}
	return name
}

func stringKey(values map[string]any, key string) string {
	text, _ := values[key].(string)
	return text
}

func vec3Key(value any) *geometry.Vec3 {
	x, y, z, ok := threeNumbers(value)
	if !ok {
		return &geometry.Vec3{}
	}
	return &geometry.Vec3{X: x, Y: y, Z: z}
}

func anglesKey(value any) *geometry.EulerAngles {
	pitch, yaw, roll, ok := threeNumbers(value)
	if !ok {
		return &geometry.EulerAngles{}
	}
	return &geometry.EulerAngles{Pitch: pitch, Yaw: yaw, Roll: roll}
}

func threeNumbers(value any) (x, y, z float64, ok bool) {
	switch v := value.(type) {
	case string:
		parts := strings.Fields(v)
		if len(parts) < 3 {
			return 0, 0, 0, false
		}
		x, errX := strconv.ParseFloat(parts[0], 64)
		y, errY := strconv.ParseFloat(parts[1], 64)
		z, errZ := strconv.ParseFloat(parts[2], 64)
		return x, y, z, errX == nil && errY == nil && errZ == nil
	default:
		items := asList(value)
		if len(items) < 3 {
			return 0, 0, 0, false
		}
		return entityNumber(items[0]), entityNumber(items[1]), entityNumber(items[2]), true
	}
}

func asList(value any) []any {
	switch v := value.(type) {
	case []any:
		return v
	default:
		return nil
	}
}

func entityNumber(value any) float64 {
	switch v := value.(type) {
	case string:
		f, _ := strconv.ParseFloat(strings.TrimSpace(v), 64)
		return f
	case float64:
		return v
	case float32:
		return float64(v)
	case int:
		return float64(v)
	case int32:
		return float64(v)
	case int64:
		return float64(v)
	case uint32:
		return float64(v)
	case json.Number:
		f, _ := v.Float64()
		return f
	case bool:
		if v {
			return 1
		}
		return 0
	default:
		return 0
	}
}

// answerEntityCall handles FindEntities, FindEntitiesByName, EntityPosition,
// EntityName, EntityClass and ReadField. It returns nil for a method this
// file does not answer.
func (s *Sandbox) answerEntityCall(call *wasm.Call) *wasm.Reply {
	if s.entities == nil {
		s.entities = newEntityIndex()
	}
	switch call.GetMethod() {
	case "FindEntities":
		request := &entities.FindEntitiesRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the FindEntities request is malformed"}
		}
		response, _ := (&entities.EntitiesResponse{Entities: s.entities.byClass[request.GetClassName()]}).MarshalVT()
		return &wasm.Reply{Response: response}
	case "FindEntitiesByName":
		request := &entities.FindEntitiesByNameRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the FindEntitiesByName request is malformed"}
		}
		response, _ := (&entities.EntitiesResponse{Entities: s.entities.byName[request.GetName()]}).MarshalVT()
		return &wasm.Reply{Response: response}
	case "EntityPosition":
		request := &entities.EntityRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the EntityPosition request is malformed"}
		}
		entity := s.entities.byHandle[request.GetEntity()]
		if entity == nil {
			return &wasm.Reply{}
		}
		response, _ := (&entities.EntityPositionResponse{
			Placement: &entities.EntityPlacement{Position: entity.position, Facing: entity.facing},
		}).MarshalVT()
		return &wasm.Reply{Response: response}
	case "EntityName":
		request := &entities.EntityRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the EntityName request is malformed"}
		}
		entity := s.entities.byHandle[request.GetEntity()]
		if entity == nil {
			return &wasm.Reply{}
		}
		response, _ := (&entities.EntityNameResponse{Name: entity.name}).MarshalVT()
		return &wasm.Reply{Response: response}
	case "EntityClass":
		request := &entities.EntityRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the EntityClass request is malformed"}
		}
		entity := s.entities.byHandle[request.GetEntity()]
		if entity == nil {
			return &wasm.Reply{}
		}
		response, _ := (&entities.EntityClassResponse{Name: &entities.EntityClassName{DesignerName: entity.class}}).MarshalVT()
		return &wasm.Reply{Response: response}
	case "ReadField":
		request := &fields.ReadFieldRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the ReadField request is malformed"}
		}
		entity := s.entities.byHandle[request.GetEntity()]
		if entity == nil {
			return &wasm.Reply{}
		}
		value, ok := readKeyValue(entity.keys, request.GetField(), request.GetType())
		if !ok {
			return &wasm.Reply{}
		}
		response, _ := (&fields.FieldResponse{Value: value}).MarshalVT()
		return &wasm.Reply{Response: response}
	default:
		return nil
	}
}

func readKeyValue(keys map[string]any, field string, kind fields.FieldType) (*fields.FieldValue, bool) {
	if field == "" || keys == nil {
		return nil, false
	}
	// A dotted schema path uses only its last component, which is what a
	// map keyvalue names.
	if i := strings.LastIndexByte(field, '.'); i >= 0 {
		field = field[i+1:]
	}
	names := []string{field}
	if mapped := schemaKeyValues[field]; mapped != "" {
		names = append(names, mapped)
	}
	var raw any
	found := false
	for _, name := range names {
		if value, ok := lookupKey(keys, name); ok {
			raw, found = value, true
			break
		}
	}
	if !found {
		return nil, false
	}
	return fieldValue(raw, kind)
}

func lookupKey(keys map[string]any, name string) (any, bool) {
	if value, ok := keys[name]; ok {
		return value, true
	}
	lower := strings.ToLower(name)
	for key, value := range keys {
		if strings.EqualFold(key, lower) {
			return value, true
		}
	}
	return nil, false
}

func fieldValue(raw any, kind fields.FieldType) (*fields.FieldValue, bool) {
	switch kind {
	case fields.FieldType_FIELD_TYPE_BOOL:
		switch v := raw.(type) {
		case bool:
			return &fields.FieldValue{Value: &fields.FieldValue_Boolean{Boolean: v}}, true
		default:
			return &fields.FieldValue{Value: &fields.FieldValue_Boolean{Boolean: entityNumber(raw) != 0}}, true
		}
	case fields.FieldType_FIELD_TYPE_STRING:
		if text, ok := raw.(string); ok {
			return &fields.FieldValue{Value: &fields.FieldValue_Text{Text: text}}, true
		}
		return nil, false
	case fields.FieldType_FIELD_TYPE_VECTOR:
		x, y, z, ok := threeNumbers(raw)
		if !ok {
			return nil, false
		}
		return &fields.FieldValue{Value: &fields.FieldValue_Vector{Vector: &geometry.Vec3{X: x, Y: y, Z: z}}}, true
	case fields.FieldType_FIELD_TYPE_INT64, fields.FieldType_FIELD_TYPE_UINT64:
		return &fields.FieldValue{Value: &fields.FieldValue_Integer{Integer: int64(entityNumber(raw))}}, true
	case fields.FieldType_FIELD_TYPE_UNKNOWN:
		return nil, false
	default:
		return &fields.FieldValue{Value: &fields.FieldValue_Number{Number: entityNumber(raw)}}, true
	}
}
