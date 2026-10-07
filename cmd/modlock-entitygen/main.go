// Command modlock-entitygen generates the typed entity classes of the
// TypeScript, Go and Luau mod libraries from a game dump, the directory
// modlock-host --dump writes:
//
//	modlock-entitygen -dump data/dump -ts js/src/entities.ts \
//		-go mod/entity -luau luau/modlock/entities.luau
//
// Every server class that derives from CEntityInstance becomes a class with
// an accessor per field the host can read: scalars, vectors, angles, entity
// handles and strings. Each field but a string can also be written. An
// accessor finds its field by name, so a game update that moves a field needs
// no new build of a mod; one that renames or drops a field needs regenerated
// classes.
//
// Each class also has a method per input it accepts that takes one value of
// a type FireInput sends, or none, and each designer name has a constructor
// over CreateEntity whose options type its spawn key values.
//
// The output depends only on the dump, so regenerating after a game update
// changes only what the update changed.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"slices"
)

func main() {
	// Read the flags; the dump and at least one output are required.
	dir := flag.String("dump", "", "game dump directory")
	ts := flag.String("ts", "", "TypeScript module to write")
	golang := flag.String("go", "", "Go package directory to write")
	luau := flag.String("luau", "", "Luau module to write")
	flag.Parse()
	if *dir == "" || (*ts == "" && *golang == "" && *luau == "") {
		flag.Usage()
		os.Exit(2)
	}

	// Generate, reporting a failure.
	if err := run(*dir, *ts, *golang, *luau); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// run reads the dump and writes each requested output.
func run(dir, ts, golang, luau string) error {
	// Read the schema's entity classes and what the entity dump adds to them.
	s, err := readSchema(dir)
	if err != nil {
		return err
	}
	classes := s.entityClasses()
	if len(classes) == 0 {
		return fmt.Errorf("%s holds no entity classes", dir)
	}
	e, err := readEntities(dir, classes)
	if err != nil {
		return err
	}

	// Write each requested output. A writer returns its files by name in the
	// output's directory, or the output itself by the empty name.
	outputs := []struct {
		path  string
		write func(*schema, []*class, *entities) map[string][]byte
	}{{ts, writeTypeScript}, {golang, writeGo}, {luau, writeLuau}}
	for _, output := range outputs {
		if output.path == "" {
			continue
		}
		for name, data := range output.write(s, classes, e) {
			if err := os.WriteFile(filepath.Join(output.path, name), data, 0o644); err != nil {
				return err
			}
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
		// Strip the fields of a base from another module.
		c := s.classes[name]
		if c.module != serverModule {
			c = &class{name: c.name, base: c.base, module: c.module}
		}

		// Visit the class, then its children in name order.
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

// readable is one field a generated accessor reads.
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
