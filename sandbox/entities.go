package sandbox

// The sandbox loads the session map's entities so FindEntities,
// FindEntitiesByName, EntityPosition, EntityName, EntityClass and ReadField
// answer without the game. It tries, for each built mod and then the project
// beside its services, the map archive maps/<map>.vpk, whose entity lumps the
// world resource lists (or default_ents when it lists none), and then the
// fixture data/sandbox-entities.json, an EntityFixture. It keeps the first
// source that reads whole and holds an entity. Names drop the prefab prefix
// [PR#] that lumps keep, because the game drops it.

import (
	"os"
	"path"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/pkg/errors"

	"github.com/paralin/modlock/source2/entitylump"
	"github.com/paralin/modlock/source2/vpk"
	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/entities"
	"github.com/paralin/modlock/src/wasm/fields"
)

// firstMapEntity is the first map entity handle, past every stand-in pawn
// handle, which start at firstEntity.
const firstMapEntity = 0x01000000

// entityFixtureFile is the name of the fixture in a data directory.
const entityFixtureFile = "sandbox-entities.json"

// schemaKeyValues maps a schema field name, as ReadField receives it, to the
// map keyvalue that stores that value. The names are generic Source 2 ones;
// a keyvalue's own name also matches.
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
	// name is the entity's targetname, as the game matches it.
	name string
	// class is its designer class.
	class string
	// position and facing place it in the world.
	position *geometry.Vec3
	facing   *geometry.EulerAngles
	// keys are its keyvalues: decoded KeyValues3 from a lump, or text from a
	// fixture.
	keys map[string]any
}

// entityIndex holds one map's entities under stable handles, addressed by
// handle, targetname or designer class.
type entityIndex struct {
	// order holds the entities by handle, from firstMapEntity.
	order   []*mapEntity
	byName  map[string][]uint32
	byClass map[string][]uint32
}

// newEntityIndex returns an index holding no entities.
func newEntityIndex() *entityIndex {
	return &entityIndex{byName: map[string][]uint32{}, byClass: map[string][]uint32{}}
}

// add records entity under the next handle.
func (idx *entityIndex) add(entity *mapEntity) {
	// Present the name as the game does, without its prefab prefix.
	if name, ok := strings.CutPrefix(entity.name, prefabPrefix); ok {
		entity.name = name
		entity.keys["targetname"] = name
	}

	handle := firstMapEntity + uint32(len(idx.order))
	idx.order = append(idx.order, entity)
	if entity.class != "" {
		idx.byClass[entity.class] = append(idx.byClass[entity.class], handle)
	}
	if entity.name != "" {
		idx.byName[entity.name] = append(idx.byName[entity.name], handle)
	}
}

// prefabPrefix is the marker Hammer puts before the targetname of an entity
// placed from a prefab, such as [PR#]sapphire_trooper_detector. Compiled lumps
// keep it, but the game drops it at spawn: FindEntitiesByName finds the bare
// name and nothing by the prefixed one.
const prefabPrefix = "[PR#]"

// entity returns the entity with handle, or nil.
func (idx *entityIndex) entity(handle uint32) *mapEntity {
	if handle < firstMapEntity || handle-firstMapEntity >= uint32(len(idx.order)) {
		return nil
	}
	return idx.order[handle-firstMapEntity]
}

// loadMapEntities sets the session's map entities from the first source that
// reads whole and holds one, and logs a source that exists but fails.
func (s *Sandbox) loadMapEntities() {
	// List the sources, nearest the mod first.
	if s.config.Map == "" {
		return
	}
	var sources []string
	for _, mod := range s.config.Mods {
		sources = append(sources, filepath.Join(mod, "maps", s.config.Map+".vpk"), filepath.Join(mod, DataDir, entityFixtureFile))
		if s.config.Fixtures != nil {
			if services := s.config.Fixtures(mod); services != "" {
				project := filepath.Dir(services)
				sources = append(sources, filepath.Join(project, "maps", s.config.Map+".vpk"), filepath.Join(project, DataDir, entityFixtureFile))
			}
		}
	}

	// Load each into its own index, so a source that fails partway leaves
	// nothing behind.
	for _, source := range sources {
		var idx *entityIndex
		var err error
		if filepath.Ext(source) == ".vpk" {
			idx, err = loadMapArchive(source, s.config.Map)
		} else {
			idx, err = loadEntityFixture(source)
		}
		switch {
		case os.IsNotExist(errors.Cause(err)):
		case err != nil:
			s.log("", "map entities: "+source+": "+err.Error())
		case len(idx.order) > 0:
			s.entities = idx
			s.log("", "map entities: loaded "+strconv.Itoa(len(idx.order))+" from "+source)
			return
		}
	}
}

// loadMapArchive reads the entities of mapName's entity lumps in the map
// archive at archivePath.
func loadMapArchive(archivePath, mapName string) (*entityIndex, error) {
	// Open the archive and list the world's lumps.
	archive, err := vpk.OpenArchive(archivePath)
	if err != nil {
		return nil, err
	}
	var lumps []string
	if world, err := entitylump.Data(archive.ReadFile, path.Join("maps", mapName, "world.vwrld_c")); err == nil {
		lumps = entitylump.Lumps(world)
	}
	if len(lumps) == 0 {
		lumps = []string{path.Join("maps", mapName, "entities", "default_ents.vents_c")}
	}

	// Walk every lump, refusing the archive if one does not read.
	idx := newEntityIndex()
	for _, lump := range lumps {
		err := entitylump.Walk(archive.ReadFile, lump, func(values map[string]any) {
			idx.add(&mapEntity{
				name:     textKey(values["targetname"]),
				class:    textKey(values["classname"]),
				position: vec3Of(values["origin"]),
				facing:   anglesOf(values["angles"]),
				keys:     values,
			})
		})
		if err != nil {
			return nil, errors.Wrap(err, lump)
		}
	}
	return idx, nil
}

// loadEntityFixture reads the EntityFixture at file.
func loadEntityFixture(file string) (*entityIndex, error) {
	// Read the fixture.
	data, err := os.ReadFile(file)
	if err != nil {
		return nil, err
	}
	fixture := &EntityFixture{}
	if err := fixture.UnmarshalJSON(data); err != nil {
		return nil, err
	}

	// Index its entities, their names and classes readable as keyvalues as
	// in a lump.
	idx := newEntityIndex()
	for _, item := range fixture.GetEntities() {
		keys := map[string]any{"targetname": item.GetTargetname(), "classname": item.GetClassname()}
		for key, value := range item.GetKeyvalues() {
			keys[key] = value
		}
		idx.add(&mapEntity{
			name:     item.GetTargetname(),
			class:    item.GetClassname(),
			position: copyVec3(item.GetOrigin()),
			facing:   copyAngles(item.GetAngles()),
			keys:     keys,
		})
	}
	return idx, nil
}

// answerEntityCall handles FindEntities, FindEntitiesByName, EntityPosition,
// EntityName, EntityClass and ReadField. It returns nil for a method this
// file does not answer.
func (s *Sandbox) answerEntityCall(call *wasm.Call) *wasm.Reply {
	// Answer the lookups.
	idx := s.entities
	method := call.GetMethod()
	var response interface{ MarshalVT() ([]byte, error) }
	switch method {
	case "FindEntities":
		request := &entities.FindEntitiesRequest{}
		if request.UnmarshalVT(call.GetRequest()) != nil {
			return malformed(method)
		}
		response = &entities.EntitiesResponse{Entities: idx.byClass[request.GetClassName()]}
	case "FindEntitiesByName":
		request := &entities.FindEntitiesByNameRequest{}
		if request.UnmarshalVT(call.GetRequest()) != nil {
			return malformed(method)
		}
		response = &entities.EntitiesResponse{Entities: idx.byName[request.GetName()]}
	case "EntityPosition", "EntityName", "EntityClass":
		request := &entities.EntityRequest{}
		if request.UnmarshalVT(call.GetRequest()) != nil {
			return malformed(method)
		}
		entity := idx.entity(request.GetEntity())
		if entity == nil {
			return &wasm.Reply{Error: "no live entity has that handle"}
		}
		switch method {
		case "EntityPosition":
			response = &entities.EntityPositionResponse{Placement: &entities.EntityPlacement{
				Position: copyVec3(entity.position),
				Facing:   copyAngles(entity.facing),
			}}
		case "EntityName":
			response = &entities.EntityNameResponse{Name: entity.name}
		default:
			response = &entities.EntityClassResponse{Name: &entities.EntityClassName{DesignerName: entity.class}}
		}
	case "ReadField":
		request := &fields.ReadFieldRequest{}
		if request.UnmarshalVT(call.GetRequest()) != nil {
			return malformed(method)
		}
		value, err := idx.readField(request)
		if err != "" {
			return &wasm.Reply{Error: err}
		}
		response = &fields.FieldResponse{Value: value}
	default:
		return nil
	}
	data, _ := response.MarshalVT()
	return &wasm.Reply{Response: data}
}

// malformed answers a request that does not decode.
func malformed(method string) *wasm.Reply {
	return &wasm.Reply{Error: "the " + method + " request is malformed"}
}

// readField reads a schema field from the keyvalue that stores it, or gives
// the reason it cannot, as the host words it.
func (idx *entityIndex) readField(request *fields.ReadFieldRequest) (*fields.FieldValue, string) {
	// Find the entity and the keyvalue. A dotted schema path names the
	// keyvalue by its last part.
	entity := idx.entity(request.GetEntity())
	if entity == nil {
		return nil, "no live entity has that handle"
	}
	if request.GetType() == fields.FieldType_FIELD_TYPE_UNKNOWN {
		return nil, "the field type is unknown"
	}
	field := request.GetField()
	if dot := strings.LastIndexByte(field, '.'); dot >= 0 {
		field = field[dot+1:]
	}
	raw, ok := keyValue(entity.keys, field)
	if !ok {
		raw, ok = keyValue(entity.keys, schemaKeyValues[field])
	}
	if !ok {
		return nil, "the map gives the entity no keyvalue for " + request.GetClassName() + "." + request.GetField()
	}

	// Read it as the type the mod asked for.
	value := fieldValue(raw, request.GetType())
	if value == nil {
		return nil, "the value does not fit the field type"
	}
	return value, ""
}

// keyValue returns the keyvalue named name, matched without case as Hammer
// matches keys.
func keyValue(keys map[string]any, name string) (any, bool) {
	if name == "" {
		return nil, false
	}
	if value, ok := keys[name]; ok {
		return value, true
	}
	for key, value := range keys {
		if strings.EqualFold(key, name) {
			return value, true
		}
	}
	return nil, false
}

// fieldValue reads a keyvalue as kind, or returns nil when it does not fit.
func fieldValue(raw any, kind fields.FieldType) *fields.FieldValue {
	switch kind {
	case fields.FieldType_FIELD_TYPE_BOOL:
		if text, ok := raw.(string); ok {
			if flag, err := strconv.ParseBool(strings.TrimSpace(text)); err == nil {
				return &fields.FieldValue{Value: &fields.FieldValue_Boolean{Boolean: flag}}
			}
		}
		return &fields.FieldValue{Value: &fields.FieldValue_Boolean{Boolean: entitylump.Number(raw) != 0}}
	case fields.FieldType_FIELD_TYPE_STRING:
		if text, ok := raw.(string); ok {
			return &fields.FieldValue{Value: &fields.FieldValue_Text{Text: text}}
		}
		return nil
	case fields.FieldType_FIELD_TYPE_VECTOR:
		x, y, z, ok := threeNumbers(raw)
		if !ok {
			return nil
		}
		return &fields.FieldValue{Value: &fields.FieldValue_Vector{Vector: &geometry.Vec3{X: x, Y: y, Z: z}}}
	case fields.FieldType_FIELD_TYPE_INT64, fields.FieldType_FIELD_TYPE_UINT64:
		return &fields.FieldValue{Value: &fields.FieldValue_Integer{Integer: int64(entitylump.Number(raw))}}
	default:
		return &fields.FieldValue{Value: &fields.FieldValue_Number{Number: entitylump.Number(raw)}}
	}
}

// textKey returns a keyvalue that is text, or empty.
func textKey(value any) string {
	text, _ := value.(string)
	return text
}

// vec3Of reads a lump's position, or the origin when it is missing.
func vec3Of(value any) *geometry.Vec3 {
	x, y, z, _ := threeNumbers(value)
	return &geometry.Vec3{X: x, Y: y, Z: z}
}

// anglesOf reads a lump's pitch, yaw and roll, or none when they are missing.
func anglesOf(value any) *geometry.EulerAngles {
	pitch, yaw, roll, _ := threeNumbers(value)
	return &geometry.EulerAngles{Pitch: pitch, Yaw: yaw, Roll: roll}
}

// threeNumbers reads a vector keyvalue: a decoded array, or text such as
// "0 90 0", as Hammer writes vectors.
func threeNumbers(value any) (x, y, z float64, ok bool) {
	var parts []any
	switch v := value.(type) {
	case string:
		for _, field := range strings.Fields(v) {
			parts = append(parts, field)
		}
	case []any:
		parts = v
	}
	if len(parts) < 3 {
		return 0, 0, 0, false
	}
	return entitylump.Number(parts[0]), entitylump.Number(parts[1]), entitylump.Number(parts[2]), true
}
