package main

import (
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"

	"github.com/paralin/modlock/proto/modlock/dump"
	"github.com/pkg/errors"
)

// serverModule is the schema module that holds the server's entity classes.
const serverModule = "server"

// class is one class the schema declares.
type class struct {
	// name is the class name.
	name string
	// base is the class it derives from, or empty.
	base string
	// module is the schema module that declares it.
	module string
	// boxed marks a class that wraps one scalar, such as GameTime_t.
	boxed bool
	// fields are the class's own fields in declaration order.
	fields []field
}

// field is one field a class declares.
type field struct {
	// name is the field's schema name, such as m_iHealth.
	name string
	// typ is the field's declared C++ type, such as CHandle< CBaseEntity >.
	typ string
}

// schema is every class and enum of one game dump.
type schema struct {
	// classes maps each class name to its declaration, preferring the
	// server module's when several modules declare one name.
	classes map[string]*class
	// enums maps each enum name to its underlying integer type.
	enums map[string]string
}

// enumTypes maps an enum's storage size to its underlying integer type.
var enumTypes = map[uint32]string{1: "uint8", 2: "uint16", 4: "uint32", 8: "uint64"}

// readSchema reads schemas.json from a game dump directory.
func readSchema(dir string) (*schema, error) {
	// Decode the dump.
	data, err := os.ReadFile(filepath.Join(dir, "schemas.json"))
	if err != nil {
		return nil, err
	}
	var dumped dump.Schemas
	if err := dumped.UnmarshalJSON(data); err != nil {
		return nil, errors.Wrap(err, "decode schemas.json")
	}

	// Index the enums and classes, the server's declaration winning a name
	// several modules declare.
	s := &schema{classes: map[string]*class{}, enums: map[string]string{}}
	for _, e := range dumped.GetEnums() {
		s.enums[e.GetName()] = enumTypes[e.GetSize()]
	}
	for _, dc := range dumped.GetClasses() {
		if existing, ok := s.classes[dc.GetName()]; ok && existing.module == serverModule {
			continue
		}
		c := &class{
			name:   dc.GetName(),
			module: dc.GetModule(),
			boxed: slices.ContainsFunc(dc.GetMetadata(), func(m string) bool {
				return strings.HasPrefix(m, "MIsBoxed")
			}),
		}
		if bases := dc.GetBases(); len(bases) != 0 {
			c.base = bases[0].GetName()
		}
		for _, f := range dc.GetFields() {
			c.fields = append(c.fields, field{name: f.GetName(), typ: f.GetType()})
		}
		s.classes[c.name] = c
	}
	return s, nil
}

// entityRoot is the class every entity derives from.
const entityRoot = "CEntityInstance"

// isEntity reports whether the class named name derives from the entity root.
func (s *schema) isEntity(name string) bool {
	for name != "" {
		if name == entityRoot {
			return true
		}
		c, ok := s.classes[name]
		if !ok {
			return false
		}
		name = c.base
	}
	return false
}

// kind is how the host reads one field: the wire field type and, for
// angles and handles, how a library presents it.
type kind struct {
	// wire is the field type the host reads, such as "int32".
	wire string
	// angles marks a vector the library presents as pitch, yaw and roll.
	angles bool
	// target names the entity class a handle points to, or is empty.
	target string
}

// scalars maps each schema scalar type to its wire field type.
var scalars = map[string]string{
	"bool":            "bool",
	"int8":            "int8",
	"int16":           "int16",
	"int32":           "int32",
	"int64":           "int64",
	"uint8":           "uint8",
	"uint16":          "uint16",
	"uint32":          "uint32",
	"uint64":          "uint64",
	"float32":         "float32",
	"float64":         "float64",
	"Vector":          "vector",
	"VectorWS":        "vector",
	"CEntityHandle":   "handle",
	"CUtlString":      "string",
	"CUtlSymbolLarge": "string",
}

// handleType matches a typed entity handle.
var handleType = regexp.MustCompile(`^CHandle< (\w+) >$`)

// kindOf resolves a field's declared type to how the host reads it, and
// reports false for a type the host cannot read, such as a container.
func (s *schema) kindOf(typ string) (kind, bool) {
	// A scalar, angle or handle reads directly.
	if wire, ok := scalars[typ]; ok {
		return kind{wire: wire}, true
	}
	if typ == "QAngle" {
		return kind{wire: "vector", angles: true}, true
	}
	if m := handleType.FindStringSubmatch(typ); m != nil {
		k := kind{wire: "handle"}
		if s.isEntity(m[1]) {
			k.target = m[1]
		}
		return k, true
	}

	// An enum reads as the unsigned integer of its size.
	if underlying, ok := s.enums[typ]; ok {
		wire, ok := scalars[underlying]
		return kind{wire: wire}, ok
	}

	// A boxed class, such as GameTime_t, reads as the scalar it wraps.
	if c, ok := s.classes[typ]; ok && c.boxed && len(c.fields) == 1 {
		return s.kindOf(c.fields[0].typ)
	}
	return kind{}, false
}
