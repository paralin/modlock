package main

import (
	"bytes"
	"fmt"
	"go/format"
	"strings"
	"unicode"
)

// goRoot declares the entity root, whose layout every class shares.
const goRoot = `
// CEntityInstance is the root of every entity class: one entity's handle.
type CEntityInstance struct {
	// Handle names the entity, as mod.Pawn.Entity does.
	Handle uint32
}
`

// goFiles names the file of each class that roots a large subtree. Every
// other class goes in its base's file, and the root in classes.go, so a game
// update moves a class between files only when it changes the class's base.
var goFiles = map[string]string{
	"CBaseModelEntity":    "models.go",
	"CCitadelBaseAbility": "abilities.go",
}

// goNumbers maps each numeric wire field type to its Go type. A handle to
// a class the libraries lack is the uint32 the host reads it as.
var goNumbers = map[string]string{
	"int8":    "int8",
	"int16":   "int16",
	"int32":   "int32",
	"uint8":   "uint8",
	"uint16":  "uint16",
	"uint32":  "uint32",
	"int64":   "int64",
	"uint64":  "uint64",
	"float32": "float32",
	"float64": "float64",
	"handle":  "uint32",
}

// goFields maps each other wire field type to its field type in package
// entity.
var goFields = map[string]string{
	"bool":   "Bool",
	"string": "Text",
	"vector": "Vector",
}

// goValues maps each value to the type a key or input takes. A key value
// struct holds a pointer to each.
var goValues = map[value]string{
	"boolean": "bool",
	"integer": "int32",
	"number":  "float32",
	"text":    "string",
	"vector":  "*mod.Vector",
	"color":   "uint32",
	"angles":  "*mod.Angles",
}

// writeGo writes packages entity and console as Go source files. Package
// entity holds the classes with their field accessors, split by goFiles, the
// classes' input methods, the key value structs and the designer names. Each
// field is one method returning a value that gets and sets the field. A class
// is a defined type over the root rather than embedding its base: the
// compiler writes a wrapper for every promoted method of every class, which
// for the game's classes would be hundreds of thousands of functions. Package
// console holds a value per console variable and command.
func writeGo(s *schema, classes []*class, e *entities, c *console) map[string][]byte {
	// Write each class after the root, in its subtree's file.
	sources := map[string]*bytes.Buffer{"classes.go": bytes.NewBufferString(goRoot)}
	fileOf := map[string]string{classes[0].name: "classes.go"}
	methods := map[*class]map[string]bool{}
	for _, c := range classes[1:] {
		// Write the class.
		file := goFiles[c.name]
		if file == "" {
			file = fileOf[c.base]
		} else {
			sources[file] = &bytes.Buffer{}
		}
		fileOf[c.name] = file
		out := sources[file]
		fmt.Fprintf(out, "\ntype %s CEntityInstance\n", c.name)

		// Write a method per field. A method may not share its name with the
		// root's Handle, and two fields may map to one method name; keep the
		// first.
		taken := map[string]bool{"Handle": true}
		methods[c] = taken
		fields := s.readableFields(c)
		if len(fields) != 0 {
			fmt.Fprintf(out, "\nfunc (e %s) field(name string) Field {\n", c.name)
			fmt.Fprintf(out, "\treturn Field{e.Handle, %q, name}\n}\n", c.name)
		}
		for _, f := range fields {
			method := goMethod(f.name)
			if taken[method] {
				continue
			}
			taken[method] = true
			typ, value := goField(f)
			fmt.Fprintf(out, "\nfunc (e %s) %s() %s {\n\treturn %s\n}\n", c.name, method, typ, value)
		}
	}
	files := map[string][]byte{}
	for file, out := range sources {
		files["entity/"+file] = goSource("entity", out.Bytes(), false)
	}

	// Write a method per input, unless a field took its name.
	var out bytes.Buffer
	for _, c := range classes[1:] {
		for _, in := range e.inputs[c.name] {
			method := "Input" + in.name
			if methods[c][method] {
				continue
			}
			methods[c][method] = true
			out.WriteString("\n")
			if doc := sentence(in.description); doc != "" {
				out.WriteString(wrap("// ", method+" sends "+in.name+". "+doc))
			}
			parameter, argument := "", "nil"
			if in.value != "" {
				parameter, argument = "value "+goValues[in.value], "value"
			}
			fmt.Fprintf(&out, "func (e %s) %s(%s) (bool, error) {\n", c.name, method, parameter)
			fmt.Fprintf(&out, "\treturn mod.FireInput(e.Handle, %q, %s, nil)\n}\n", in.name, argument)
		}
	}
	files["entity/inputs.go"] = goSource("entity", out.Bytes(), true)

	// Write each data description's key value struct, which embeds its
	// base's, so each key is declared once and a mod sets a base's key as a
	// promoted field.
	out.Reset()
	for _, m := range e.keyMaps {
		fmt.Fprintf(&out, "\ntype %sKeys struct {\n", m.name)
		named := map[string]bool{}
		if m.base != nil {
			fmt.Fprintf(&out, "\t%sKeys\n", m.base.name)
			named[m.base.name+"Keys"] = true
		}
		for _, k := range m.keys {
			name := camel(goMethod(k.name))
			if named[name] {
				continue
			}
			named[name] = true
			typ := goValues[k.value]
			if !strings.HasPrefix(typ, "*") {
				typ = "*" + typ
			}
			fmt.Fprintf(&out, "\t%s %s `key:%q`\n", name, typ, k.name)
		}
		out.WriteString("}\n")
	}
	files["entity/keys.go"] = goSource("entity", out.Bytes(), true)

	// Write each designer name, unless a class took its name. Each is its
	// own declaration, which gofmt does not align with its neighbors, so a
	// new name changes only its own line.
	out.Reset()
	out.WriteString("\n")
	for _, d := range e.designers {
		name := camel(d.name)
		if s.classes[name] != nil {
			continue
		}
		fmt.Fprintf(&out, "var %s = Designer[%s, %sKeys]{%q, %q}\n", name, d.class, d.keys.name, d.name, d.subclass)
	}
	files["entity/designers.go"] = goSource("entity", out.Bytes(), false)

	// Write each console variable and command, unless another took its
	// name.
	taken := map[string]bool{}
	consoleFiles := []struct {
		name    string
		entries []consoleEntry
	}{{"console/variables.go", c.variables}, {"console/commands.go", c.commands}}
	for _, file := range consoleFiles {
		out.Reset()
		for _, entry := range file.entries {
			name := camel(entry.name)
			if taken[name] {
				continue
			}
			taken[name] = true
			out.WriteString("\n")
			if doc := sentence(entry.help); doc != "" {
				out.WriteString(wrap("// ", name+": "+doc))
			}
			if entry.typ == "" {
				fmt.Fprintf(&out, "var %s = Command{%q}\n", name, entry.name)
			} else {
				fmt.Fprintf(&out, "var %s = Variable[%s]{%q}\n", name, entry.typ, entry.name)
			}
		}
		files[file.name] = goSource("console", out.Bytes(), false)
	}
	return files
}

// goSource returns body as a formatted generated source file of package pkg,
// importing package mod when the body uses it.
func goSource(pkg string, body []byte, imports bool) []byte {
	header := "// Code generated by modlock-entitygen. DO NOT EDIT.\n\npackage " + pkg + "\n"
	if imports {
		header += "\nimport \"github.com/paralin/modlock/mod\"\n"
	}
	formatted, err := format.Source(append([]byte(header), body...))
	if err != nil {
		panic(fmt.Sprintf("generated Go does not parse: %v", err))
	}
	return formatted
}

// goMethod returns the method name for a schema field name: the name without
// its m_ prefix, capitalized, so m_iHealth is IHealth.
func goMethod(field string) string {
	name := []rune(strings.TrimPrefix(field, "m_"))
	name[0] = unicode.ToUpper(name[0])
	if !unicode.IsLetter(name[0]) {
		return "F" + string(name)
	}
	return string(name)
}

// goField returns the type of field f and the expression that addresses it
// on entity e.
func goField(f readable) (typ, value string) {
	switch {
	case f.angles:
		typ = "Angles"
	case f.target != "":
		typ = "Handle[" + f.target + "]"
	case goFields[f.wire] != "":
		typ = goFields[f.wire]
	default:
		typ = "Number[" + goNumbers[f.wire] + "]"
	}
	return typ, fmt.Sprintf("%s{e.field(%q)}", typ, f.name)
}
