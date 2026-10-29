package panorama

import (
	"bytes"
	"encoding/xml"
	"io"
	"regexp"
	"strings"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/pkg/errors"
)

// element is one parsed layout element.
type element struct {
	// name is the element's name, such as Panel or include.
	name string
	// attributes are the element's attributes in source order.
	attributes []xml.Attr
	// children are the nested elements.
	children []*element
	// text is the body of an inline script.
	text string
	// line and column are the 1-based position of the element's name.
	line, column int
}

// parse reads a layout's single root element. Comments, declarations and
// text outside inline scripts are ignored, and HTML entities such as &nbsp;
// are accepted.
func parse(source []byte) (*element, error) {
	// Read the XML, tracking each token's position.
	decoder := xml.NewDecoder(bytes.NewReader(source))
	decoder.Entity = xml.HTMLEntity
	at := position(source)

	// Read elements, keeping the stack of open ones under a holder for the
	// root.
	holder := &element{}
	stack := []*element{holder}
	for {
		offset := decoder.InputOffset()
		token, err := decoder.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		top := stack[len(stack)-1]
		switch token := token.(type) {
		case xml.StartElement:
			// The name follows the opening bracket.
			line, column := at(int(offset) + 1)
			child := &element{name: qualified(token.Name), attributes: token.Attr, line: line, column: column}
			for i, attribute := range child.attributes {
				child.attributes[i].Name = xml.Name{Local: qualified(attribute.Name)}
			}
			top.children = append(top.children, child)
			stack = append(stack, child)
		case xml.EndElement:
			stack = stack[:len(stack)-1]
		case xml.CharData:
			if top.name == "script" {
				top.text += string(token)
			}
		}
	}
	if len(stack) != 1 {
		return nil, errors.Errorf("<%s> is not closed", stack[len(stack)-1].name)
	}
	if len(holder.children) != 1 {
		return nil, errors.New("A layout has exactly one root element")
	}
	return holder.children[0], nil
}

// qualified returns name as written, with its prefix when it has one.
func qualified(name xml.Name) string {
	if name.Space == "" {
		return name.Local
	}
	return name.Space + ":" + name.Local
}

// position returns a function that gives the 1-based line and column of
// increasing byte offsets in source.
func position(source []byte) func(offset int) (int, int) {
	line, lineStart, counted := 1, 0, 0
	return func(offset int) (int, int) {
		offset = min(offset, len(source))
		for ; counted < offset; counted++ {
			if source[counted] == '\n' {
				line, lineStart = line+1, counted+1
			}
		}
		return line, offset - lineStart + 1
	}
}

// node converts an element to its layout tree node. A node holds one child
// as child and several as vecChildren.
func (e *element) node() kv3.Object {
	// Convert the child elements.
	children := make([]any, 0, len(e.children))
	for _, child := range e.children {
		children = append(children, child.node())
	}
	at := []any{e.line, e.column}

	// Name the node by its element.
	switch e.name {
	case "root":
		return withChildren(kv3.Object{{Key: "eType", Value: "ROOT"}}, children, []any{})
	case "styles", "scripts", "snippets":
		return withChildren(kv3.Object{{Key: "eType", Value: strings.ToUpper(e.name)}}, children, at)
	case "snippet":
		return withChildren(kv3.Object{{Key: "eType", Value: "SNIPPET"}, {Key: "name", Value: e.attribute("name")}}, children, at)
	case "include":
		return kv3.Object{{Key: "eType", Value: "INCLUDE"}, {Key: "child", Value: reference(e.attribute("src"))}, {Key: "sourceLineColumn", Value: at}}
	case "script":
		return kv3.Object{{Key: "eType", Value: "SCRIPT_BODY"}, {Key: "name", Value: e.text}, {Key: "sourceLineColumn", Value: at}}
	}

	// A panel holds its attributes before its children.
	attributes := make([]any, 0, len(e.attributes)+len(children))
	for _, attribute := range e.attributes {
		// An empty value has no name.
		value := kv3.Object{{Key: "eType", Value: "PANEL_ATTRIBUTE_VALUE"}}
		switch {
		case referencePattern.MatchString(attribute.Value):
			value = reference(attribute.Value)
		case attribute.Value != "":
			value = append(value, kv3.Member{Key: "name", Value: attribute.Value})
		}
		attributes = append(attributes, kv3.Object{{Key: "eType", Value: "PANEL_ATTRIBUTE"}, {Key: "name", Value: attribute.Name.Local}, {Key: "child", Value: value}})
	}
	return withChildren(kv3.Object{{Key: "eType", Value: "PANEL"}, {Key: "name", Value: e.name}}, append(attributes, children...), at)
}

// attribute returns the value of the attribute name, or "".
func (e *element) attribute(name string) string {
	for _, attribute := range e.attributes {
		if attribute.Name.Local == name {
			return attribute.Value
		}
	}
	return ""
}

// withChildren appends a node's children and source position to its fields.
func withChildren(node kv3.Object, children []any, at []any) kv3.Object {
	switch len(children) {
	case 0:
	case 1:
		node = append(node, kv3.Member{Key: "child", Value: children[0]})
	default:
		node = append(node, kv3.Member{Key: "vecChildren", Value: children})
	}
	return append(node, kv3.Member{Key: "sourceLineColumn", Value: at})
}

// referencePattern matches an attribute value naming a resource.
var referencePattern = regexp.MustCompile(`^(s2r|file)://`)

// resourcesPattern matches a file in the layout's own resources.
var resourcesPattern = regexp.MustCompile(`^file://\{resources\}/(.*)\.(\w+)$`)

// reference converts an include or attribute path to its compiled reference.
// A file under {resources} is compiled with the layout, so it is named by its
// compiled resource; a video plays from its own file.
func reference(value string) kv3.Object {
	if name, ok := strings.CutPrefix(value, "s2r://"); ok {
		return kv3.Object{{Key: "eType", Value: "REFERENCE_COMPILED"}, {Key: "name", Value: name}}
	}
	if match := resourcesPattern.FindStringSubmatch(value); match != nil && match[2] != "webm" {
		return kv3.Object{{Key: "eType", Value: "REFERENCE_COMPILED"}, {Key: "name", Value: compiledName("panorama/" + match[1] + "." + match[2])}}
	}
	return kv3.Object{{Key: "eType", Value: "REFERENCE_PASSTHROUGH"}, {Key: "name", Value: strings.TrimPrefix(value, "file://")}}
}
