package studio

import (
	"net/http"
	"slices"
	"strings"
)

// world answers a compiled map's World: every model its world nodes place,
// read from the map's world.vwrld_c and each node it names.
func (a *Assets) world(w http.ResponseWriter, r *http.Request) {
	// Read the world's list of nodes.
	_, blocks, err := a.read(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	data, err := decodeTag(blocks, "DATA")
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnprocessableEntity)
		return
	}

	// Place each node's scene objects and aggregates; a node that cannot be
	// read is left out.
	b := worldBuilder{assets: a, out: &World{Materials: map[string]*Material{}}, models: map[string]uint32{}}
	for _, item := range list(data["m_worldNodes"]) {
		node, _ := item.(map[string]any)
		prefix, _ := node["m_worldNodePrefix"].(string)
		if prefix == "" {
			continue
		}
		_, nodeBlocks, err := a.read(strings.ReplaceAll(prefix, "\\", "/") + ".vwnod_c")
		if err != nil {
			continue
		}
		if nodeData, err := decodeTag(nodeBlocks, "DATA"); err == nil {
			b.node(nodeData)
		}
	}
	b.out.Lighting = a.lighting(data)
	writeMessage(w, b.out)
}

// worldBuilder gathers a World from its nodes, naming each model once.
type worldBuilder struct {
	assets *Assets
	out    *World
	models map[string]uint32
}

// model returns the index in World.models of the model a node names
// without its _c suffix.
func (b *worldBuilder) model(name string) uint32 {
	name += "_c"
	index, ok := b.models[name]
	if !ok {
		index = uint32(len(b.out.Models))
		b.models[name] = index
		b.out.Models = append(b.out.Models, name)
	}
	return index
}

// node adds one world node's placements.
func (b *worldBuilder) node(data map[string]any) {
	// Scene objects draw a whole model at a transform.
	objects := list(data["m_sceneObjects"])
	placed := make([]*Placement, len(objects))
	for i, item := range objects {
		object, _ := item.(map[string]any)
		name, _ := object["m_renderableModel"].(string)
		if name == "" {
			continue
		}
		placed[i] = &Placement{
			Model:     b.model(name),
			Transform: rows(object["m_vTransform"]),
			Tint:      floats(object["m_vTintColor"], 1),
		}
		b.out.Placements = append(b.out.Placements, placed[i])
	}

	// Material overrides name a scene object's draw call.
	for _, item := range list(data["m_materialOverrides"]) {
		override, _ := item.(map[string]any)
		index := int(number(override["m_nSceneObjectIndex"]))
		material, _ := override["m_pMaterial"].(string)
		if index < 0 || index >= len(placed) || placed[index] == nil || material == "" {
			continue
		}
		if placed[index].Materials == nil {
			placed[index].Materials = map[uint32]string{}
		}
		placed[index].Materials[uint32(number(override["m_nDrawCallIndex"]))] = material
		if _, ok := b.out.Materials[material]; !ok {
			b.out.Materials[material] = b.assets.material(material)
		}
	}

	// Aggregates draw chosen draw calls of one merged model.
	for _, item := range list(data["m_aggregateSceneObjects"]) {
		aggregate, _ := item.(map[string]any)
		if name, _ := aggregate["m_renderableModel"].(string); name != "" {
			b.aggregate(name, aggregate)
		}
	}
}

// aggregate adds one aggregate scene object: each of its meshes that draws
// at the finest level of detail, taking the fragment transforms in order by
// the meshes that have one.
func (b *worldBuilder) aggregate(name string, aggregate map[string]any) {
	placement := &Placement{Model: b.model(name)}
	transforms := list(aggregate["m_fragmentTransforms"])
	next := 0
	for _, item := range list(aggregate["m_aggregateMeshes"]) {
		m, _ := item.(map[string]any)
		fragment := &Fragment{
			DrawCall: uint32(number(m["m_nDrawCallIndex"])),
			Tint:     floats(m["m_vTintColor"], 255),
		}
		if m["m_bHasTransform"] == true && next < len(transforms) {
			fragment.Transform = floats(transforms[next], 1)
			next++
		}
		// A zero mask draws at every level of detail.
		if lods := uint64(number(m["m_nLODGroupMask"])); lods == 0 || lods&1 != 0 {
			placement.Fragments = append(placement.Fragments, fragment)
		}
	}
	if len(placement.Fragments) != 0 {
		b.out.Placements = append(b.out.Placements, placement)
	}
}

// rows flattens a decoded 3x4 matrix of rows into twelve floats, or returns
// nil for the identity or a malformed matrix.
func rows(value any) []float32 {
	var out []float32
	for _, row := range list(value) {
		out = append(out, floats(row, 1)...)
	}
	identity := []float32{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}
	if len(out) != 12 || slices.Equal(out, identity) {
		return nil
	}
	return out
}

// floats returns a decoded list of numbers divided by scale, or nil for an
// empty list or one of nothing but scale, which tints white.
func floats(value any, scale float64) []float32 {
	items := list(value)
	out := make([]float32, len(items))
	white := true
	for i, item := range items {
		out[i] = float32(number(item) / scale)
		white = white && out[i] == 1
	}
	if white {
		return nil
	}
	return out
}
