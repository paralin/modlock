package main

import (
	"iter"
	"maps"
)

// bindings is the immutable rendering input shared by the language writers.
// Its streams preserve declaration order and can be traversed repeatedly.
// Writers must not mutate the borrowed declarations, parts, events or membership sets.
type bindings struct {
	// directions records the public input and output types.
	*directions
	// file declares the common call and reply envelopes.
	file *protoFile
	// calls contains the resolved generated Host calls.
	calls []*call
	// events contains Mod methods in declaration order.
	events []*method
	// parts contains the validated Host partition in file order.
	parts []*part
	// hostDoc documents the Host service.
	hostDoc string
	// modDoc documents the Mod service.
	modDoc string
	// messages traverses the public messages in declaration order.
	messages iter.Seq[*message]
	// enums traverses the public enums in declaration order.
	enums iter.Seq[*enum]
	// goBindings contains the protobuf alias and union projections.
	*goBindings
	// absent identifies singular messages whose missing value stays absent.
	absent map[*field]bool
	// pythonEnums includes dictionary-only library enum tables.
	pythonEnums iter.Seq2[*enum, bool]
	// pythonMessages traverses the Python codec declarations.
	pythonMessages iter.Seq[*message]
	// luauMessages traverses the Luau codec declarations.
	luauMessages iter.Seq[*message]
	// luauEnums retains first-reached order for additional codec enum tables.
	luauEnums []*enum
}

// goBindings is the immutable Go-specific projection of resolved calls.
type goBindings struct {
	// goMessages includes the protobuf wrappers Go aliases.
	goMessages iter.Seq[*message]
	// goEnums includes the enums reached by Go's protobuf wrappers.
	goEnums iter.Seq[*enum]
	// goUnions records which conversions each union needs.
	goUnions map[*message]conversion
}

// conversion records the required directions of a generated union codec.
type conversion struct {
	// encode requires a public-to-protobuf conversion.
	encode bool
	// decode requires a protobuf-to-public conversion.
	decode bool
}

// bind resolves public types, codec roots and target-specific projections.
// Writers consume this immutable result without opening schema collections.
func (s *schema) bind() (*bindings, error) {
	// Resolve the common callable surface and directional public types.
	calls, err := s.calls()
	if err != nil {
		return nil, err
	}
	d := s.directions(calls)
	b := &bindings{
		directions: d, goBindings: s.bindGo(calls), file: s.file, calls: calls, events: s.mod.methods,
		parts: s.parts, hostDoc: s.host.doc, modDoc: s.mod.doc,
		messages: selected(s.messageOrder, d.inputs, d.outputs),
		enums:    selected(s.enumOrder, d.enumsIn, d.enumsOut),
		absent:   map[*field]bool{},
	}

	// Preserve missing singular results and service fields in script codecs.
	for _, c := range calls {
		if f := c.result; f != nil && f.message != nil && !f.repeated && !f.message.isBareUnion() {
			b.absent[f] = true
		}
	}
	var services reach
	for _, m := range s.services {
		services.add(m)
	}
	services.markAbsent(b.absent)

	// Resolve Python's dictionary-only roots independently of public classes.
	var library reach
	for _, m := range s.host.methods {
		if skipped[m.name] {
			library.add(m.request)
			library.add(m.response)
		}
	}
	for _, m := range s.services {
		library.add(m)
	}
	b.pythonEnums = s.pythonEnums(d, library.enums)

	// Seed both script codecs with the envelopes, calls and Mod events.
	var python, luau reach
	for _, m := range s.messageOrder {
		if m.name == "Call" || m.name == "Reply" || m.name == "Empty" {
			python.add(m)
			luau.add(m)
		}
	}
	for _, c := range calls {
		python.add(c.method.request)
		python.add(c.method.response)
		luau.add(c.method.request)
		luau.add(c.method.response)
	}
	for _, m := range s.host.methods {
		if m.name == "Ui" {
			luau.add(m.request)
			luau.add(m.response)
		}
	}
	for _, m := range s.mod.methods {
		python.add(m.request)
		python.add(m.response)
		luau.add(m.request)
		luau.add(m.response)
	}

	// Add each language's handwritten roots without changing traversal order.
	python.include(&library)
	for _, m := range s.services {
		luau.add(m)
	}
	b.pythonMessages = selected(s.messageOrder, set(python.messages))
	b.luauMessages = selected(s.messageOrder, set(luau.messages))
	b.luauEnums = luau.enums
	return b, nil
}

// bindGo resolves the protobuf wrappers and union conversions used by Go.
func (s *schema) bindGo(calls []*call) *goBindings {
	// Initialize the projection before resolving its reachable types.
	g := &goBindings{}

	// Include Go's protobuf wrappers even when their public call is flattened.
	var in, out reach
	for _, c := range calls {
		if c.options {
			in.add(c.method.request)
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
		in.add(m.response)
		out.add(m.request)
	}

	// Classify union conversion directions once for all Go writer passes.
	inputs, outputs := set(in.messages), set(out.messages)
	g.goMessages = selected(s.messageOrder, inputs, outputs)
	g.goUnions = make(map[*message]conversion)
	for m := range g.goMessages {
		if m.isUnion() {
			g.goUnions[m] = conversion{encode: inputs[m], decode: outputs[m]}
		}
	}
	g.goEnums = selected(s.enumOrder, set(in.enums), set(out.enums))

	return g
}

// pythonEnums streams public enum declarations and handwritten-only tables.
func (s *schema) pythonEnums(d *directions, library []*enum) iter.Seq2[*enum, bool] {
	// Classify public enums once, then retain library-only wire tables.
	public := maps.Clone(d.enumsIn)
	maps.Copy(public, d.enumsOut)
	enums := selected(s.enumOrder, public, set(library))
	return func(yield func(*enum, bool) bool) {
		for e := range enums {
			if !yield(e, public[e]) {
				return
			}
		}
	}
}

// selected streams items present in any membership set in declaration order.
// It keeps the input storage and membership sets immutable while traversing.
func selected[T comparable](items []T, memberships ...map[T]bool) iter.Seq[T] {
	return func(yield func(T) bool) {
		for _, item := range items {
			for _, membership := range memberships {
				if !membership[item] {
					continue
				}
				if !yield(item) {
					return
				}
				break
			}
		}
	}
}
