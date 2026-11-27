// Package entitylump reads the entities a compiled Source 2 map stores.
//
// A map's world resource, maps/<map>/world.vwrld_c, lists its entity lumps
// in m_entityLumps. Each lump is a compiled resource whose DATA block holds
// KeyValues3 with m_entityKeyValues, one entry per entity whose
// keyValues3Data.values are its keyvalues, and m_childLumps, the lumps nested
// under it.
package entitylump

import (
	"strconv"
	"strings"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
)

// maxDepth is how deep child lumps nest before Walk stops descending; maps
// nest them only a few deep.
const maxDepth = 8

// Lumps returns the compiled resource names of the entity lumps a world's
// decoded DATA block lists. The world names each lump without its _c.
func Lumps(world map[string]any) []string {
	var names []string
	for _, item := range list(world["m_entityLumps"]) {
		if name, _ := item.(string); name != "" {
			names = append(names, compiled(name))
		}
	}
	return names
}

// Walk calls visit with the keyvalues of each entity in the lump named name
// and in its child lumps, depth first, in stored order. read returns a
// compiled resource's bytes by name. A lump that cannot be read or decoded
// adds nothing and Walk goes on to the next; Walk returns the first such
// error, so a caller that needs every entity can refuse a partial walk.
func Walk(read func(name string) ([]byte, error), name string, visit func(values map[string]any)) error {
	return walk(read, name, visit, 0)
}

// walk visits one lump at depth, then its children.
func walk(read func(string) ([]byte, error), name string, visit func(map[string]any), depth int) error {
	// Decode the lump.
	if depth > maxDepth {
		return nil
	}
	data, err := Data(read, name)
	if err != nil {
		return err
	}

	// Visit its entities, then descend into its children.
	for _, item := range list(data["m_entityKeyValues"]) {
		entity, _ := item.(map[string]any)
		stored, _ := entity["keyValues3Data"].(map[string]any)
		if values, _ := stored["values"].(map[string]any); values != nil {
			visit(values)
		}
	}
	var first error
	for _, item := range list(data["m_childLumps"]) {
		if child, _ := item.(string); child != "" {
			if err := walk(read, compiled(child), visit, depth+1); err != nil && first == nil {
				first = err
			}
		}
	}
	return first
}

// Data reads the compiled resource named name and decodes its DATA block.
func Data(read func(name string) ([]byte, error), name string) (map[string]any, error) {
	// Read the resource and decode its DATA block as one object.
	raw, err := read(name)
	if err != nil {
		return nil, err
	}
	block, err := resource.ReadBlock(raw, "DATA")
	if err != nil {
		return nil, err
	}
	value, err := kv3.Decode(block)
	if err != nil {
		return nil, err
	}
	object, _ := value.(map[string]any)
	return object, nil
}

// Number returns an entity keyvalue as a number. Lumps store many numbers as
// text and flags as booleans; a value that is neither reads as 0.
func Number(value any) float64 {
	switch v := value.(type) {
	case string:
		f, _ := strconv.ParseFloat(strings.TrimSpace(v), 64)
		return f
	case bool:
		if v {
			return 1
		}
		return 0
	case int64:
		return float64(v)
	case uint64:
		return float64(v)
	case float64:
		return v
	}
	return 0
}

// compiled is the compiled resource name of a lump name, which a world or
// parent lump may store without the trailing _c.
func compiled(name string) string {
	if strings.HasSuffix(name, "_c") {
		return name
	}
	return name + "_c"
}

// list returns a decoded KeyValues3 array, or nil for any other value.
func list(value any) []any {
	items, _ := value.([]any)
	return items
}
