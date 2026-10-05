package main

import (
	"bufio"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/pkg/errors"
)

// serverModule is the schema module that holds the server's entity classes.
const serverModule = "server"

// class is one class a schema header declares.
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

// schema is every class and enum in a DumpSource2 schema tree.
type schema struct {
	// classes maps each class name to its declaration, preferring the
	// server module's when several modules declare one name.
	classes map[string]*class
	// enums maps each enum name to its underlying integer type.
	enums map[string]string
}

var (
	classLine = regexp.MustCompile(`^class (\w+)(?: : public (\w+))?`)
	enumLine  = regexp.MustCompile(`^enum (\w+) : (\w+)`)
	fieldLine = regexp.MustCompile(`^\t([^/\t].*?) (\w+);`)
)

// readSchema reads the headers DumpSource2 writes under root, one directory
// per module and one header per class or enum.
func readSchema(root string) (*schema, error) {
	s := &schema{classes: map[string]*class{}, enums: map[string]string{}}
	modules, err := os.ReadDir(root)
	if err != nil {
		return nil, errors.Wrap(err, "read the schema directory")
	}
	for _, module := range modules {
		if !module.IsDir() {
			continue
		}
		headers, err := filepath.Glob(filepath.Join(root, module.Name(), "*.h"))
		if err != nil {
			return nil, err
		}
		for _, header := range headers {
			if err := s.readHeader(module.Name(), header); err != nil {
				return nil, errors.Wrap(err, header)
			}
		}
	}
	return s, nil
}

// readHeader adds the class or enum one header declares.
func (s *schema) readHeader(module, path string) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()

	var declared *class
	boxed := false
	lines := bufio.NewScanner(file)
	for lines.Scan() {
		line := lines.Text()

		// Metadata comments precede the declaration they describe.
		if strings.HasPrefix(line, "// MIsBoxed") {
			boxed = true
			continue
		}
		if m := enumLine.FindStringSubmatch(line); m != nil {
			s.enums[m[1]] = m[2]
			return nil
		}
		if m := classLine.FindStringSubmatch(line); m != nil {
			declared = &class{name: m[1], base: m[2], module: module, boxed: boxed}
			continue
		}
		if m := fieldLine.FindStringSubmatch(line); declared != nil && m != nil {
			declared.fields = append(declared.fields, field{name: m[2], typ: m[1]})
		}
	}
	if err := lines.Err(); err != nil {
		return err
	}
	if declared == nil {
		return nil
	}
	if existing, ok := s.classes[declared.name]; ok && existing.module == serverModule {
		return nil
	}
	s.classes[declared.name] = declared
	return nil
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

	// An enum reads as its underlying integer, such as uint8 for uint8_t.
	if underlying, ok := s.enums[typ]; ok {
		wire, ok := scalars[strings.TrimSuffix(underlying, "_t")]
		return kind{wire: wire}, ok
	}

	// A boxed class, such as GameTime_t, reads as the scalar it wraps.
	if c, ok := s.classes[typ]; ok && c.boxed && len(c.fields) == 1 {
		return s.kindOf(c.fields[0].typ)
	}
	return kind{}, false
}
