package main

import (
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"unicode"

	"github.com/paralin/modlock/proto/modlock/dump"
	"github.com/pkg/errors"
)

// value is the case of EntityValue a key value or input takes, or how a
// library presents it: boolean, integer, number, text, vector, color, or
// angles, which travel as a vector of pitch, yaw and roll.
type value string

// keyValues maps each key type a library can set to its value. A key of
// another type, an array or an entity handle, has no typed option.
var keyValues = map[dump.KeyType]value{
	dump.KeyType_KEY_TYPE_BOOLEAN:                     "boolean",
	dump.KeyType_KEY_TYPE_INT16:                       "integer",
	dump.KeyType_KEY_TYPE_INT32:                       "integer",
	dump.KeyType_KEY_TYPE_UINT8:                       "integer",
	dump.KeyType_KEY_TYPE_UINT16:                      "integer",
	dump.KeyType_KEY_TYPE_UINT32:                      "integer",
	dump.KeyType_KEY_TYPE_TICK:                        "integer",
	dump.KeyType_KEY_TYPE_FLOAT32:                     "number",
	dump.KeyType_KEY_TYPE_TIME:                        "number",
	dump.KeyType_KEY_TYPE_STRING:                      "text",
	dump.KeyType_KEY_TYPE_UTLSTRING:                   "text",
	dump.KeyType_KEY_TYPE_UTLSTRINGTOKEN:              "text",
	dump.KeyType_KEY_TYPE_GLOBALSYMBOL:                "text",
	dump.KeyType_KEY_TYPE_SOUNDNAME:                   "text",
	dump.KeyType_KEY_TYPE_HMATERIAL:                   "text",
	dump.KeyType_KEY_TYPE_HMODEL:                      "text",
	dump.KeyType_KEY_TYPE_HRENDERTEXTURE:              "text",
	dump.KeyType_KEY_TYPE_HPARTICLESYSTEMDEFINITION:   "text",
	dump.KeyType_KEY_TYPE_HPOSTPROCESSING:             "text",
	dump.KeyType_KEY_TYPE_VECTOR:                      "vector",
	dump.KeyType_KEY_TYPE_POSITION_VECTOR:             "vector",
	dump.KeyType_KEY_TYPE_DIRECTION_VECTOR_WORLDSPACE: "vector",
	dump.KeyType_KEY_TYPE_NETWORK_QUANTIZED_VECTOR:    "vector",
	dump.KeyType_KEY_TYPE_COLOR32:                     "color",
	dump.KeyType_KEY_TYPE_QANGLE:                      "angles",
	dump.KeyType_KEY_TYPE_QANGLE_WORLDSPACE:           "angles",
}

// parameters maps each Pulse type an input can take from FireInput to its
// value. An input taking another type, such as an entity handle, has no
// typed method.
var parameters = map[string]value{
	"PVAL_BOOL":            "boolean",
	"PVAL_INT":             "integer",
	"PVAL_FLOAT":           "number",
	"PVAL_STRING":          "text",
	"PVAL_ENTITY_NAME":     "text",
	"PVAL_VEC3":            "vector",
	"PVAL_VEC3_WORLDSPACE": "vector",
	"PVAL_COLOR_RGB":       "color",
}

// identifier matches a key or input name every library can spell as a
// property or method, which leaves out patterns such as position%d.
var identifier = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

// key is one spawn key value a library sets through a typed option.
type key struct {
	// name is the key, such as rendercolor.
	name string
	// value is how the key's value travels.
	value value
}

// keyMap is one data description: the key values it adds to its base's.
type keyMap struct {
	// name is the data description's class name, such as CBaseModelEntity.
	name string
	// base is the base data description, or nil.
	base *keyMap
	// keys are the keys the map adds, embedded maps' included, leaving out a
	// name a base already has.
	keys []key
}

// all returns every key of the map and its bases, the map's own first.
func (m *keyMap) all() []key {
	var keys []key
	for ; m != nil; m = m.base {
		keys = append(keys, m.keys...)
	}
	return keys
}

// has reports whether the map or a base has the key name.
func (m *keyMap) has(name string) bool {
	return slices.ContainsFunc(m.all(), func(k key) bool { return k.name == name })
}

// designer is one designer name a mod creates through a typed constructor.
type designer struct {
	// name is the designer name, such as npc_trooper_boss.
	name string
	// class is the schema class that implements it.
	class string
	// keys is its data description.
	keys *keyMap
}

// input is one input a typed method sends.
type input struct {
	// name is the input's name, such as Alpha.
	name string
	// description is the input's help text, or empty.
	description string
	// value is how the input's value travels, or empty for an input that
	// takes none.
	value value
}

// entities is what a game dump's entities.json adds to the schema's
// classes: the designer names, their key values and the classes' inputs.
type entities struct {
	// designers are sorted by name.
	designers []designer
	// keyMaps are the data descriptions the designers reach, each after its
	// base.
	keyMaps []*keyMap
	// inputs maps each schema class to the inputs it accepts and its bases
	// do not.
	inputs map[string][]input
}

// everyEntity is the class that accepts the inputs every entity accepts.
const everyEntity = "CBaseEntity"

// readEntities reads entities.json from a game dump directory and keeps the
// designer names whose class is one of classes.
func readEntities(dir string, classes []*class) (*entities, error) {
	// Decode the dump.
	data, err := os.ReadFile(filepath.Join(dir, "entities.json"))
	if err != nil {
		return nil, err
	}
	var dumped dump.Entities
	if err := dumped.UnmarshalJSON(data); err != nil {
		return nil, errors.Wrap(err, "decode entities.json")
	}

	// Index the dump.
	known := map[string]*class{}
	for _, c := range classes {
		known[c.name] = c
	}
	maps := map[string]*dump.DataMap{}
	for _, m := range dumped.GetDataMaps() {
		maps[m.GetName()] = m
	}
	classOf := map[string]string{"": everyEntity}
	e := &entities{inputs: map[string][]input{}}
	resolved := map[string]*keyMap{}
	var resolve func(name string) *keyMap
	resolve = func(name string) *keyMap {
		// Resolve each map once, after its base.
		if m, ok := resolved[name]; ok || name == "" {
			return m
		}
		dm := maps[name]
		m := &keyMap{name: name, base: resolve(dm.GetBase())}
		resolved[name] = m
		e.keyMaps = append(e.keyMaps, m)

		// Gather its keys and its embedded maps' keys, an embedded map's with
		// its bases'.
		var gather func(dm *dump.DataMap, bases bool)
		gather = func(dm *dump.DataMap, bases bool) {
			for ; dm != nil; dm = maps[dm.GetBase()] {
				for _, kv := range dm.GetKeyValues() {
					v, ok := keyValues[kv.GetType()]
					if !ok || kv.GetCount() != 1 || !identifier.MatchString(kv.GetName()) ||
						m.has(kv.GetName()) {
						continue
					}
					m.keys = append(m.keys, key{name: kv.GetName(), value: v})
				}
				for _, embedded := range dm.GetEmbedded() {
					gather(maps[embedded.GetDataMap()], true)
				}
				if !bases {
					return
				}
			}
		}
		gather(dm, false)
		return m
	}

	// Keep each designer name whose class the libraries have and that has a
	// data description, which leaves out the root.
	for _, c := range dumped.GetClasses() {
		classOf[c.GetDesignerName()] = c.GetClassName()
		if known[c.GetClassName()] == nil || c.GetDataMap() == "" ||
			!identifier.MatchString(c.GetDesignerName()) {
			continue
		}
		e.designers = append(e.designers, designer{
			name:  c.GetDesignerName(),
			class: c.GetClassName(),
			keys:  resolve(c.GetDataMap()),
		})
	}

	// File each input under its class, leaving out one a library cannot send
	// and a query that returns its result only to Pulse.
	for _, in := range dumped.GetInputs() {
		class := classOf[in.GetEntity()]
		if known[class] == nil || !identifier.MatchString(in.GetName()) || len(in.GetReturns()) != 0 {
			continue
		}
		next := input{name: in.GetName(), description: in.GetDescription()}
		switch params := in.GetParameters(); len(params) {
		case 0:
		case 1:
			v, ok := parameters[params[0].GetType()]
			if !ok {
				continue
			}
			next.value = v
		default:
			continue
		}
		if !slices.ContainsFunc(e.inputs[class], func(i input) bool { return i.name == next.name }) {
			e.inputs[class] = append(e.inputs[class], next)
		}
	}
	return e, nil
}

// doc returns the input's description as a sentence, or empty when the game
// gives none.
func (in input) doc() string {
	description := strings.TrimSuffix(strings.TrimSpace(in.description), ".")
	if description == "" {
		return ""
	}
	return description + "."
}

// wrap breaks text into lines of at most 80 columns, each starting with
// prefix and ending in a newline.
func wrap(prefix, text string) string {
	var b strings.Builder
	line := prefix
	for _, word := range strings.Fields(text) {
		if line != prefix && len(line)+1+len(word) > 80 {
			b.WriteString(line + "\n")
			line = prefix
		}
		if line != prefix {
			line += " "
		}
		line += word
	}
	b.WriteString(line + "\n")
	return b.String()
}

// camel joins a snake_case name's words, each capitalized, so
// npc_trooper_boss is NpcTrooperBoss.
func camel(name string) string {
	words := strings.Split(name, "_")
	for i, word := range words {
		if word != "" {
			runes := []rune(word)
			runes[0] = unicode.ToUpper(runes[0])
			words[i] = string(runes)
		}
	}
	return strings.Join(words, "")
}
