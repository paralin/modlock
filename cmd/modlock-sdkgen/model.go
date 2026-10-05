package main

import (
	"fmt"
	"strings"
	"unicode"

	"github.com/aperturerobotics/protobuf-go-lite/types/descriptorpb"
)

// schema is the boundary the two services declare, with every message and
// enum they reach.
type schema struct {
	// messages holds each message by its full name, such as ".modlock.Vec3".
	messages map[string]*message
	// enums holds each enum by its full name.
	enums map[string]*enum
	// messageOrder and enumOrder list the messages and enums in schema
	// order, which the generated files follow.
	messageOrder []*message
	enumOrder    []*enum
	// host is the service the game serves to mods.
	host *service
	// mod is the service a mod serves to the game.
	mod *service
}

// message is one protobuf message.
type message struct {
	name   string
	full   string
	doc    string
	fields []*field
	oneofs []*oneof
}

// field is one field of a message.
type field struct {
	// name is the field's name in the schema, such as max_health.
	name     string
	number   int32
	doc      string
	kind     descriptorpb.FieldDescriptorProto_Type
	repeated bool
	// optional is true for a field marked optional.
	optional bool
	// oneof is the oneof holding the field, or nil.
	oneof *oneof
	// message and enum are the field's type when it has one.
	message *message
	enum    *enum
}

// oneof is one oneof of a message, which a field outside the oneof never
// shares.
type oneof struct {
	name   string
	doc    string
	fields []*field
}

// enum is one protobuf enum.
type enum struct {
	name   string
	full   string
	doc    string
	values []*enumValue
}

// enumValue is one value of an enum.
type enumValue struct {
	// name is the value's name in the schema, such as SCREEN_EFFECT_BLACK.
	name   string
	number int32
	doc    string
}

// service is one service of the schema.
type service struct {
	name    string
	doc     string
	methods []*method
}

// method is one method of a service.
type method struct {
	name     string
	doc      string
	request  *message
	response *message
}

// readSchema builds the schema from a compiled descriptor set that includes
// source info, taking the services from the file named file.
func readSchema(set *descriptorpb.FileDescriptorSet, file string) (*schema, error) {
	s := &schema{messages: map[string]*message{}, enums: map[string]*enum{}}
	type pending struct {
		field    *field
		typeName string
	}
	var types []pending
	var services []*descriptorpb.ServiceDescriptorProto
	var serviceDocs map[string]string
	var serviceFile *descriptorpb.FileDescriptorProto
	for _, f := range set.GetFile() {
		docs := comments(f)
		prefix := "." + f.GetPackage() + "."
		var addEnum func(e *descriptorpb.EnumDescriptorProto, name, full, path string)
		addEnum = func(e *descriptorpb.EnumDescriptorProto, name, full, path string) {
			en := &enum{name: name, full: full, doc: docs[path]}
			for j, v := range e.GetValue() {
				en.values = append(en.values, &enumValue{name: v.GetName(), number: v.GetNumber(), doc: docs[fmt.Sprint(path, ".2.", j)]})
			}
			s.enums[en.full] = en
			s.enumOrder = append(s.enumOrder, en)
		}
		var addMessage func(m *descriptorpb.DescriptorProto, name, full, path string)
		addMessage = func(m *descriptorpb.DescriptorProto, name, full, path string) {
			msg := &message{name: name, full: full, doc: docs[path]}
			for j, o := range m.GetOneofDecl() {
				msg.oneofs = append(msg.oneofs, &oneof{name: o.GetName(), doc: docs[fmt.Sprint(path, ".8.", j)]})
			}
			for j, fd := range m.GetField() {
				fl := &field{
					name:     fd.GetName(),
					number:   fd.GetNumber(),
					doc:      docs[fmt.Sprint(path, ".2.", j)],
					kind:     fd.GetType(),
					repeated: fd.GetLabel() == descriptorpb.FieldDescriptorProto_LABEL_REPEATED,
					optional: fd.GetProto3Optional(),
				}
				if fd.OneofIndex != nil && !fl.optional {
					fl.oneof = msg.oneofs[fd.GetOneofIndex()]
					fl.oneof.fields = append(fl.oneof.fields, fl)
				}
				if fd.GetTypeName() != "" {
					types = append(types, pending{fl, fd.GetTypeName()})
				}
				msg.fields = append(msg.fields, fl)
			}
			msg.oneofs = realOneofs(msg.oneofs)
			s.messages[msg.full] = msg
			s.messageOrder = append(s.messageOrder, msg)
			for j, nested := range m.GetNestedType() {
				addMessage(nested, name+"_"+nested.GetName(), full+"."+nested.GetName(), fmt.Sprint(path, ".3.", j))
			}
			for j, nested := range m.GetEnumType() {
				addEnum(nested, name+"_"+nested.GetName(), full+"."+nested.GetName(), fmt.Sprint(path, ".4.", j))
			}
		}
		for i, m := range f.GetMessageType() {
			addMessage(m, m.GetName(), prefix+m.GetName(), fmt.Sprint(4, ".", i))
		}
		for i, e := range f.GetEnumType() {
			addEnum(e, e.GetName(), prefix+e.GetName(), fmt.Sprint(5, ".", i))
		}
		if f.GetName() == file {
			services, serviceDocs, serviceFile = f.GetService(), docs, f
		}
	}
	if serviceFile == nil {
		return nil, fmt.Errorf("the descriptor set lacks %s", file)
	}

	// Resolve each field's message or enum type.
	for _, p := range types {
		p.field.message, p.field.enum = s.messages[p.typeName], s.enums[p.typeName]
		if p.field.message == nil && p.field.enum == nil {
			return nil, fmt.Errorf("field %s has unknown type %s", p.field.name, p.typeName)
		}
	}

	// Read the services.
	prefix := "." + serviceFile.GetPackage() + "."
	for i, sd := range services {
		svc := &service{name: sd.GetName(), doc: serviceDocs[fmt.Sprint(6, ".", i)]}
		for j, md := range sd.GetMethod() {
			m := &method{
				name:     md.GetName(),
				doc:      serviceDocs[fmt.Sprint(6, ".", i, ".2.", j)],
				request:  s.messages[md.GetInputType()],
				response: s.messages[md.GetOutputType()],
			}
			if m.request == nil || m.response == nil || md.GetClientStreaming() || md.GetServerStreaming() {
				return nil, fmt.Errorf("method %s.%s needs unary messages", svc.name, m.name)
			}
			svc.methods = append(svc.methods, m)
		}
		switch svc.name {
		case "Host":
			s.host = svc
		case "Mod":
			s.mod = svc
		}
	}
	if s.host == nil || s.mod == nil {
		return nil, fmt.Errorf("%s must declare the Host and Mod services", file)
	}
	if s.messages[prefix+"Empty"] == nil {
		return nil, fmt.Errorf("%s must declare Empty", file)
	}
	return s, nil
}

// realOneofs drops the synthetic oneofs that proto3 optional fields carry.
func realOneofs(oneofs []*oneof) []*oneof {
	var real []*oneof
	for _, o := range oneofs {
		if len(o.fields) != 0 {
			real = append(real, o)
		}
	}
	return real
}

// comments returns the leading comment of each element of f by its source
// path, such as "4.2" for the third message.
func comments(f *descriptorpb.FileDescriptorProto) map[string]string {
	docs := map[string]string{}
	for _, location := range f.GetSourceCodeInfo().GetLocation() {
		text := location.GetLeadingComments()
		if text == "" {
			continue
		}
		parts := make([]string, len(location.GetPath()))
		for i, step := range location.GetPath() {
			parts[i] = fmt.Sprint(step)
		}
		lines := strings.Split(strings.TrimRight(text, "\n"), "\n")
		for i, line := range lines {
			lines[i] = strings.TrimRight(strings.TrimPrefix(line, " "), " ")
		}
		docs[strings.Join(parts, ".")] = strings.Join(lines, "\n")
	}
	return docs
}

// isEmpty reports whether m has no fields.
func (m *message) isEmpty() bool {
	return len(m.fields) == 0
}

// isUnion reports whether m holds only one oneof, which each language
// represents as a union of its cases' types.
func (m *message) isUnion() bool {
	return len(m.oneofs) == 1 && len(m.oneofs[0].fields) == len(m.fields)
}

// class is a kind of thing a mod addresses by a number the host gives it.
// A request whose first field is the class's field calls a method of the
// class.
type class struct {
	// name is the class's name in every language.
	name string
	doc  string
	// field is the field name that holds the class's number.
	field string
	// key is the member that holds the number, in lowerCamel form, and
	// goKey and goType are its Go name and type.
	key    string
	keyDoc string
	goKey  string
	goType string
	// noun is the word a method name drops, such as Object in MoveObject.
	noun string
}

// classes are the classes of the mod libraries.
var classes = []*class{
	{
		name:   "Player",
		doc:    "Player addresses one connected player by their server slot.",
		field:  "player",
		key:    "slot",
		goKey:  "Slot",
		goType: "int32",
		keyDoc: "slot identifies the player on the server.",
		noun:   "Player",
	},
	{
		name:   "WorldObject",
		doc:    "WorldObject is one object the mod placed in the world.",
		field:  "object",
		key:    "id",
		goKey:  "ID",
		goType: "uint32",
		keyDoc: "id identifies the object among the mod's objects.",
		noun:   "Object",
	},
	{
		name:   "Npc",
		doc:    "Npc is one unit the mod spawned.",
		field:  "npc",
		key:    "entity",
		goKey:  "Entity",
		goType: "uint32",
		keyDoc: "entity is the unit's entity handle, as damage events report it.",
		noun:   "Npc",
	},
	{
		name:   "Pickup",
		doc:    "Pickup is one pickup the mod placed.",
		field:  "pickup",
		key:    "entity",
		goKey:  "Entity",
		goType: "uint32",
		keyDoc: "entity is the pickup's entity handle.",
		noun:   "Pickup",
	},
}

// classOf returns the class a field holds, or nil.
func classOf(f *field) *class {
	if f.message != nil || f.enum != nil || f.repeated {
		return nil
	}
	for _, c := range classes {
		if c.field == f.name {
			return c
		}
	}
	return nil
}

// skipped are Host methods the libraries call by hand instead of through a
// generated function: Ui takes the interface tree each library builds.
var skipped = map[string]bool{"Ui": true}

// call is one Host method as the libraries present it.
type call struct {
	method *method
	// receiver is the request field that makes the call a method of its
	// class, or nil for a free function.
	receiver *field
	class    *class
	// name is the call's name in lowerCamel form.
	name string
	// options is true when the request is one options argument.
	options bool
	// params are the positional arguments, without the receiver.
	params []*param
	// result is the response field the call returns, or nil when it returns
	// whether it succeeded.
	result *field
}

// param is one positional argument: a field, or a oneof standing for its
// fields.
type param struct {
	name     string
	field    *field
	oneof    *oneof
	optional bool
}

// calls returns the Host methods the libraries generate, in schema order.
func (s *schema) calls() ([]*call, error) {
	var calls []*call
	for _, m := range s.host.methods {
		if skipped[m.name] {
			continue
		}
		c := &call{method: m, name: lowerCamel(m.name)}
		fields := m.request.fields
		if len(fields) != 0 {
			if class := classOf(fields[0]); class != nil {
				c.receiver, c.class = fields[0], class
				c.name = lowerCamel(strings.Replace(m.name, class.noun, "", 1))
				fields = fields[1:]
			}
		}
		if strings.HasSuffix(m.request.name, "Options") {
			c.options = true
		} else {
			seen := map[*oneof]bool{}
			for _, f := range fields {
				switch {
				case f.oneof == nil:
					c.params = append(c.params, &param{name: lowerCamel(f.name), field: f, optional: f.optional || f.repeated})
				case !seen[f.oneof]:
					seen[f.oneof] = true
					c.params = append(c.params, &param{name: lowerCamel(f.oneof.name), oneof: f.oneof})
				}
			}
			for i := 1; i < len(c.params); i++ {
				if c.params[i-1].optional && !c.params[i].optional {
					return nil, fmt.Errorf("%s: optional %s precedes required %s", m.name, c.params[i-1].name, c.params[i].name)
				}
			}
		}
		switch len(m.response.fields) {
		case 0:
		case 1:
			c.result = m.response.fields[0]
		default:
			return nil, fmt.Errorf("%s: a response holds at most one field", m.name)
		}
		calls = append(calls, c)
	}
	return calls, nil
}

// camel returns a schema name in UpperCamel form: max_health becomes
// MaxHealth.
func camel(name string) string {
	var b strings.Builder
	upper := true
	for _, r := range name {
		switch {
		case r == '_':
			upper = true
		case upper:
			b.WriteRune(unicode.ToUpper(r))
			upper = false
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}

// lowerCamel returns a schema or method name in lowerCamel form: max_health
// and MaxHealth become maxHealth.
func lowerCamel(name string) string {
	name = camel(name)
	if name == "" {
		return ""
	}
	return strings.ToLower(name[:1]) + name[1:]
}

// valueName returns an enum value's name without its enum's prefix, in
// lowerCamel form: SCREEN_EFFECT_MATCH_INTRO becomes matchIntro.
func (e *enum) valueName(v *enumValue) string {
	short := strings.TrimPrefix(v.name, upperSnake(e.name)+"_")
	return lowerCamel(strings.ToLower(short))
}

// upperSnake returns an UpperCamel name in UPPER_SNAKE form.
func upperSnake(name string) string {
	var b strings.Builder
	for i, r := range name {
		if i != 0 && unicode.IsUpper(r) {
			b.WriteByte('_')
		}
		b.WriteRune(unicode.ToUpper(r))
	}
	return b.String()
}

// named returns values whose names a mod uses: every value but the unset
// *_UNKNOWN.
func (e *enum) named() []*enumValue {
	var values []*enumValue
	for _, v := range e.values {
		if !strings.HasSuffix(v.name, "_UNKNOWN") {
			values = append(values, v)
		}
	}
	return values
}

// rename replaces the leading word of doc, the schema's name for what it
// describes, with the name a language gives it.
func rename(doc, from, to string) string {
	if rest, ok := strings.CutPrefix(doc, from); ok && (rest == "" || rest[0] == ' ') {
		return to + rest
	}
	return doc
}

// reach collects the messages and enums a set of root messages reaches
// through their fields, in first-reached order.
type reach struct {
	messages []*message
	enums    []*enum
	seen     map[any]bool
}

// add adds m and everything it reaches.
func (r *reach) add(m *message) {
	if r.seen == nil {
		r.seen = map[any]bool{}
	}
	if r.seen[m] {
		return
	}
	r.seen[m] = true
	r.messages = append(r.messages, m)
	for _, f := range m.fields {
		r.addField(f)
	}
}

// addField adds the type of f and everything it reaches.
func (r *reach) addField(f *field) {
	switch {
	case f.message != nil:
		r.add(f.message)
	case f.enum != nil && !r.seen[f.enum]:
		r.seen[f.enum] = true
		r.enums = append(r.enums, f.enum)
	}
}

// directions holds the messages and enums a mod writes and reads, which the
// libraries write types for.
type directions struct {
	// inputs and outputs are the messages a mod writes and reads.
	inputs, outputs map[*message]bool
	// omitted holds the receiver field an options message leaves out.
	omitted map[*message]*field
	// enumsIn and enumsOut are the enums a mod writes and reads.
	enumsIn, enumsOut map[*enum]bool
}

// directions finds the messages and enums each direction of calls and the
// Mod service carries.
func (s *schema) directions(calls []*call) *directions {
	d := &directions{omitted: map[*message]*field{}}
	var in, out reach
	for _, c := range calls {
		if c.options {
			in.add(c.method.request)
			if c.receiver != nil {
				d.omitted[c.method.request] = c.receiver
			}
		}
		for _, p := range c.params {
			for _, f := range p.fields() {
				in.addField(f)
			}
		}
		if c.result != nil {
			out.addField(c.result)
		}
	}
	for _, m := range s.mod.methods {
		if !m.request.isEmpty() {
			out.add(m.request)
		}
		if !m.response.isEmpty() {
			in.add(m.response)
		}
	}
	d.inputs, d.outputs = set(in.messages), set(out.messages)
	d.enumsIn, d.enumsOut = set(in.enums), set(out.enums)
	return d
}

// fields returns the fields a parameter stands for.
func (p *param) fields() []*field {
	if p.oneof != nil {
		return p.oneof.fields
	}
	return []*field{p.field}
}

// set returns the members of items as a set.
func set[T comparable](items []T) map[T]bool {
	s := map[T]bool{}
	for _, item := range items {
		s[item] = true
	}
	return s
}
