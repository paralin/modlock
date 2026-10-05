// Command modlock-entitygen generates the typed entity classes of the
// TypeScript and Go mod libraries from the server schema headers that the
// DumpSource2 tool writes, one directory per module under its schemas
// directory:
//
//	modlock-entitygen -schemas DumpSource2/schemas \
//		-ts js/src/entities.ts -go mod/entity/entity.go
//
// Every server class that derives from CEntityInstance becomes a class with
// one getter per field the host can read: scalars, vectors, angles, entity
// handles and strings. A getter reads its field by name, so a game update
// that moves a field needs no new build of a mod; one that renames or drops a
// field needs regenerated classes.
package main

import (
	"flag"
	"fmt"
	"os"
	"slices"
)

func main() {
	schemas := flag.String("schemas", "", "DumpSource2 schemas directory")
	ts := flag.String("ts", "", "TypeScript module to write")
	golang := flag.String("go", "", "Go source file to write")
	flag.Parse()
	if *schemas == "" || (*ts == "" && *golang == "") {
		flag.Usage()
		os.Exit(2)
	}
	if err := run(*schemas, *ts, *golang); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// run reads the schema and writes each requested output.
func run(schemas, ts, golang string) error {
	s, err := readSchema(schemas)
	if err != nil {
		return err
	}
	classes := s.entityClasses()
	if len(classes) == 0 {
		return fmt.Errorf("%s holds no entity classes", schemas)
	}
	if ts != "" {
		if err := os.WriteFile(ts, writeTypeScript(s, classes), 0o644); err != nil {
			return err
		}
	}
	if golang != "" {
		if err := os.WriteFile(golang, writeGo(s, classes), 0o644); err != nil {
			return err
		}
	}
	return nil
}

// entityClasses returns the entity root and every server entity class, each
// after its base, with siblings in name order.
func (s *schema) entityClasses() []*class {
	// Group the server's entity classes, and the bases they derive through,
	// under their bases.
	children := map[string][]string{}
	grouped := map[string]bool{entityRoot: true}
	for name, c := range s.classes {
		if c.module != serverModule || !s.isEntity(name) {
			continue
		}
		for !grouped[name] {
			base := s.classes[name].base
			grouped[name] = true
			children[base] = append(children[base], name)
			name = base
		}
	}

	// Walk the hierarchy from the root. A base from another module holds no
	// server fields, so it appears without them.
	var ordered []*class
	var walk func(name string)
	walk = func(name string) {
		c := s.classes[name]
		if c.module != serverModule {
			c = &class{name: c.name, base: c.base, module: c.module}
		}
		ordered = append(ordered, c)
		names := children[name]
		slices.Sort(names)
		for _, child := range names {
			walk(child)
		}
	}
	walk(entityRoot)
	return ordered
}

// readable is one field a generated getter reads.
type readable struct {
	field
	kind
}

// readableFields returns the fields of c the host can read.
func (s *schema) readableFields(c *class) []readable {
	var fields []readable
	for _, f := range c.fields {
		if k, ok := s.kindOf(f.typ); ok {
			fields = append(fields, readable{field: f, kind: k})
		}
	}
	return fields
}
