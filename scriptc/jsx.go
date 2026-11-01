package scriptc

import (
	"bytes"
	"encoding/json"
	"html"
	"strings"

	"github.com/pkg/errors"
)

// jsxImport binds the library's jsx in a lowered file. It joins the file's
// first line, so every line keeps its number.
const jsxImport = "import { jsx as __modlockJsx, type JSX as __ModlockJSX } from 'modlock'; "

// fragmentType is the type of the library's fragment element, which holds a
// component's elements or a fragment's children.
const fragmentType = `"fragment"`

// LowerJSX rewrites the JSX in one TSX source file as calls to the modlock
// library's jsx, since scriptc compiles TypeScript but not JSX. An element
// becomes jsx(type, props, key). A component renders in place inside a
// fragment, and a fragment element holds its children, as the library's jsx
// builds them at run time, so both runtimes build the same tree. Each element
// keeps the lines it spanned, so the code after it keeps its line numbers.
func LowerJSX(source []byte) ([]byte, error) {
	// Lower the file's code, naming the line where it fails.
	l := &lowerer{src: source}
	var out strings.Builder
	if _, err := l.code(&out, false); err != nil {
		return nil, errors.Wrapf(err, "line %d", l.line())
	}

	// Import jsx only into a file that calls it.
	if !l.lowered {
		return source, nil
	}
	return []byte(jsxImport + out.String()), nil
}

// lowerer scans TypeScript source, copying it through and rewriting each JSX
// element it meets.
type lowerer struct {
	// src is the source being lowered.
	src []byte
	// pos is the offset of the next byte to read.
	pos int
	// lowered reports whether any element was rewritten.
	lowered bool
}

// line returns the line number of pos, for errors.
func (l *lowerer) line() int {
	return bytes.Count(l.src[:min(l.pos, len(l.src))], []byte("\n")) + 1
}

// code copies code to out until the end of the source or, when nested, the
// unmatched closing brace of an expression container or template
// substitution, which it leaves unread. It reports whether the code held any
// token besides whitespace and comments.
//
// Whether a slash or an angle bracket starts a regular expression or an
// element depends on the token before it: after an operand it is an operator.
func (l *lowerer) code(out *strings.Builder, nested bool) (bool, error) {
	depth, operand, tokens := 0, false, false
	for l.pos < len(l.src) {
		c := l.src[l.pos]

		// Copy whitespace and comments, which change no state.
		if isSpace(c) {
			out.WriteByte(c)
			l.pos++
			continue
		}
		if l.has("//") || l.has("/*") {
			if err := l.comment(out); err != nil {
				return tokens, err
			}
			continue
		}

		// End a nested expression at its unmatched closing brace.
		if c == '}' && depth == 0 && nested {
			return tokens, nil
		}

		// Copy one token, noting whether it ends an operand.
		tokens = true
		switch {
		case c == '"' || c == '\'':
			if err := l.quoted(out); err != nil {
				return tokens, err
			}
			operand = true
		case c == '`':
			if err := l.template(out); err != nil {
				return tokens, err
			}
			operand = true
		case c == '/' && !operand:
			if err := l.regexp(out); err != nil {
				return tokens, err
			}
			operand = true
		case c == '<' && !operand && l.startsElement():
			call, err := l.element()
			if err != nil {
				return tokens, err
			}
			out.WriteString(call)
			operand = true
		case c == '{' || c == '(' || c == '[':
			depth++
			out.WriteByte(c)
			l.pos++
			operand = false
		case c == '}' || c == ')' || c == ']':
			depth--
			out.WriteByte(c)
			l.pos++
			operand = c != '}'
		case isWordByte(c):
			word := l.word()
			out.WriteString(word)
			operand = !expressionKeywords[word]
		case c == '!' && operand:
			// A non-null assertion leaves its operand an operand.
			out.WriteByte(c)
			l.pos++
		default:
			out.WriteByte(c)
			l.pos++
			operand = false
		}
	}
	if nested {
		return tokens, errors.New("unclosed expression")
	}
	return tokens, nil
}

// expressionKeywords are the keywords an expression may follow, so a slash or
// an angle bracket after one starts an operand.
var expressionKeywords = map[string]bool{
	"await": true, "case": true, "default": true, "delete": true, "do": true,
	"else": true, "in": true, "instanceof": true, "new": true, "of": true,
	"return": true, "throw": true, "typeof": true, "void": true, "yield": true,
}

// has reports whether the source at pos starts with prefix.
func (l *lowerer) has(prefix string) bool {
	return bytes.HasPrefix(l.src[l.pos:], []byte(prefix))
}

// comment copies one line or block comment.
func (l *lowerer) comment(out *strings.Builder) error {
	// Find where the comment ends: a line comment at its newline, which
	// stays outside it, or at the end of the source.
	end, closing := "\n", 0
	if l.has("/*") {
		end, closing = "*/", 2
	}
	n := bytes.Index(l.src[l.pos+2:], []byte(end))
	if n < 0 {
		if closing != 0 {
			return errors.New("unclosed comment")
		}
		n = len(l.src) - l.pos - 2
	}

	// Copy it.
	stop := l.pos + 2 + n + closing
	out.Write(l.src[l.pos:stop])
	l.pos = stop
	return nil
}

// quoted copies one string literal.
func (l *lowerer) quoted(out *strings.Builder) error {
	start, quote := l.pos, l.src[l.pos]
	for l.pos++; l.pos < len(l.src); l.pos++ {
		switch l.src[l.pos] {
		case '\\':
			l.pos++
		case quote:
			l.pos++
			out.Write(l.src[start:l.pos])
			return nil
		case '\n':
			return errors.New("unclosed string")
		}
	}
	return errors.New("unclosed string")
}

// template copies one template literal, lowering the code it substitutes.
func (l *lowerer) template(out *strings.Builder) error {
	out.WriteByte('`')
	for l.pos++; l.pos < len(l.src); {
		switch {
		case l.src[l.pos] == '\\' && l.pos+1 < len(l.src):
			out.Write(l.src[l.pos : l.pos+2])
			l.pos += 2
		case l.src[l.pos] == '`':
			out.WriteByte('`')
			l.pos++
			return nil
		case l.has("${"):
			out.WriteString("${")
			l.pos += 2
			if _, err := l.code(out, true); err != nil {
				return err
			}
			out.WriteByte('}')
			l.pos++
		default:
			out.WriteByte(l.src[l.pos])
			l.pos++
		}
	}
	return errors.New("unclosed template")
}

// regexp copies one regular expression literal and its flags.
func (l *lowerer) regexp(out *strings.Builder) error {
	start, class := l.pos, false
	for l.pos++; l.pos < len(l.src); l.pos++ {
		switch c := l.src[l.pos]; {
		case c == '\\':
			l.pos++
		case c == '[':
			class = true
		case c == ']':
			class = false
		case c == '/' && !class:
			l.pos++
			for l.pos < len(l.src) && isWordByte(l.src[l.pos]) {
				l.pos++
			}
			out.Write(l.src[start:l.pos])
			return nil
		case c == '\n':
			return errors.New("unclosed regular expression")
		}
	}
	return errors.New("unclosed regular expression")
}

// word reads one identifier, keyword or number.
func (l *lowerer) word() string {
	start := l.pos
	for l.pos < len(l.src) && isWordByte(l.src[l.pos]) {
		l.pos++
	}
	return string(l.src[start:l.pos])
}

// startsElement reports whether the angle bracket at pos opens an element or a
// fragment rather than a generic arrow function's type parameters, which TSX
// writes as <T,> or <T extends U>.
func (l *lowerer) startsElement() bool {
	// A fragment opens with <>, and an element with a name.
	rest := l.src[l.pos+1:]
	if len(rest) == 0 {
		return false
	}
	if rest[0] == '>' {
		return true
	}
	if !isWordByte(rest[0]) {
		return false
	}

	// Type parameters follow their first name with a comma or extends.
	name := 0
	for name < len(rest) && isNameByte(rest[name]) {
		name++
	}
	after := bytes.TrimLeft(rest[name:], " \t\r\n")
	return !bytes.HasPrefix(after, []byte(",")) && !bytes.HasPrefix(after, []byte("extends "))
}

// element reads one element or fragment at pos and returns its call, padded
// with the newlines the element spanned beyond the call's own.
func (l *lowerer) element() (string, error) {
	// Read the element.
	start := l.pos
	call, err := l.elementCall()
	if err != nil {
		return "", err
	}
	l.lowered = true

	// Pad the call to the element's lines.
	missing := bytes.Count(l.src[start:l.pos], []byte("\n")) - strings.Count(call, "\n")
	return call + strings.Repeat("\n", max(missing, 0)), nil
}

// elementCall reads one element or fragment at pos and returns its call.
func (l *lowerer) elementCall() (string, error) {
	// Read a fragment's children.
	l.pos++
	if l.has(">") {
		l.pos++
		children, err := l.children("")
		if err != nil {
			return "", err
		}
		return call(fragmentType, props(nil, children), ""), nil
	}

	// Read the element's name and attributes.
	name := l.name()
	if name == "" {
		return "", errors.New("expected an element name")
	}
	attributes, key, closed, err := l.attributes()
	if err != nil {
		return "", errors.Wrapf(err, "<%s>", name)
	}

	// Read its children, unless it closed itself.
	var children []string
	if !closed {
		if children, err = l.children(name); err != nil {
			return "", errors.Wrapf(err, "<%s>", name)
		}
	}

	// Build an intrinsic element, or render a component in a fragment.
	if isIntrinsic(name) {
		return call(jsString(name), elementProps(name, props(attributes, children)), key), nil
	}
	rendered := name + "(" + props(attributes, children) + ")"
	return call(fragmentType, "{ children: "+rendered+" }", key), nil
}

// attributes reads an element's attributes through the end of its opening
// tag, returning each as an object literal member, its key expression, and
// whether the tag closed itself.
func (l *lowerer) attributes() (members []string, key string, closed bool, err error) {
	for {
		// End at the tag's close.
		l.skipSpace()
		switch {
		case l.pos >= len(l.src):
			return nil, "", false, errors.New("unclosed tag")
		case l.has("/>"):
			l.pos += 2
			return members, key, true, nil
		case l.has(">"):
			l.pos++
			return members, key, false, nil
		}

		// Read a spread attribute.
		if l.has("{") {
			l.pos++
			var spread strings.Builder
			if _, err := l.code(&spread, true); err != nil {
				return nil, "", false, err
			}
			l.pos++
			members = append(members, strings.TrimSpace(spread.String()))
			continue
		}

		// Read a named attribute and its value, true when it has none.
		name := l.name()
		if name == "" {
			return nil, "", false, errors.Errorf("unexpected %q in a tag", l.src[l.pos])
		}
		value := "true"
		l.skipSpace()
		if l.has("=") {
			l.pos++
			l.skipSpace()
			if value, err = l.attributeValue(); err != nil {
				return nil, "", false, errors.Wrap(err, name)
			}
		}
		if name == "key" {
			key = value
			continue
		}
		members = append(members, propertyName(name)+": "+value)
	}
}

// attributeValue reads one attribute's value: a string, an expression
// container or an element.
func (l *lowerer) attributeValue() (string, error) {
	switch {
	case l.has(`"`) || l.has("'"):
		quote := l.src[l.pos]
		end := bytes.IndexByte(l.src[l.pos+1:], quote)
		if end < 0 {
			return "", errors.New("unclosed attribute string")
		}
		value := string(l.src[l.pos+1 : l.pos+1+end])
		l.pos += end + 2
		return jsString(html.UnescapeString(value)), nil
	case l.has("{"):
		l.pos++
		var value strings.Builder
		if _, err := l.code(&value, true); err != nil {
			return "", err
		}
		l.pos++
		return strings.TrimSpace(value.String()), nil
	case l.has("<"):
		return l.element()
	}
	return "", errors.New("expected an attribute value")
}

// children reads an element's children through its closing tag for name,
// empty for a fragment, returning each as an expression.
func (l *lowerer) children(name string) ([]string, error) {
	var children []string
	for l.pos < len(l.src) {
		switch {
		case l.has("</"):
			// End at the closing tag, which must match.
			l.pos += 2
			l.skipSpace()
			closing := l.name()
			l.skipSpace()
			if closing != name || !l.has(">") {
				return nil, errors.Errorf("</%s> closes <%s>", closing, name)
			}
			l.pos++
			return children, nil
		case l.has("<"):
			child, err := l.element()
			if err != nil {
				return nil, err
			}
			children = append(children, child)
		case l.has("{"):
			// Keep an expression container's expression, unless it holds
			// only a comment.
			l.pos++
			var child strings.Builder
			tokens, err := l.code(&child, true)
			if err != nil {
				return nil, err
			}
			l.pos++
			if tokens {
				children = append(children, strings.TrimSpace(child.String()))
			}
		default:
			// Keep text as JSX trims it.
			end := l.pos
			for end < len(l.src) && l.src[end] != '<' && l.src[end] != '{' {
				end++
			}
			text := jsxText(string(l.src[l.pos:end]))
			l.pos = end
			if text != "" {
				children = append(children, jsString(text))
			}
		}
	}
	return nil, errors.New("unclosed element")
}

// name reads an element or attribute name: an identifier, a member
// expression, or a dashed or namespaced name.
func (l *lowerer) name() string {
	start := l.pos
	for l.pos < len(l.src) && isNameByte(l.src[l.pos]) {
		l.pos++
	}
	return string(l.src[start:l.pos])
}

// skipSpace skips the whitespace in a tag, which the call does not keep.
func (l *lowerer) skipSpace() {
	for l.pos < len(l.src) && isSpace(l.src[l.pos]) {
		l.pos++
	}
}

// call returns the call to the library's jsx that builds one element.
func call(kind, props, key string) string {
	if key != "" {
		return "__modlockJsx(" + kind + ", " + props + ", " + key + ")"
	}
	return "__modlockJsx(" + kind + ", " + props + ")"
}

// elementProps types an intrinsic element's props as that element's own, as
// the TypeScript compiler does for JSX, so an input's handler takes the value
// the element gives it rather than the unknown value of any input.
func elementProps(name, props string) string {
	return "(" + props + " satisfies __ModlockJSX.IntrinsicElements[" + jsString(name) + "])"
}

// props returns the object literal holding an element's attribute members
// and its children: the only child itself, or an array of several.
func props(members, children []string) string {
	switch {
	case len(children) == 1 && !strings.HasPrefix(children[0], "..."):
		members = append(members, "children: "+children[0])
	case len(children) > 0:
		members = append(members, "children: ["+strings.Join(children, ", ")+"]")
	}
	if len(members) == 0 {
		return "{}"
	}
	return "{ " + strings.Join(members, ", ") + " }"
}

// jsxText returns JSX text as an element holds it: each line trimmed where it
// meets another line, empty lines dropped, and the rest joined by spaces.
func jsxText(raw string) string {
	lines := strings.Split(strings.ReplaceAll(raw, "\r\n", "\n"), "\n")
	var kept []string
	for i, line := range lines {
		if i > 0 {
			line = strings.TrimLeft(line, " \t")
		}
		if i < len(lines)-1 {
			line = strings.TrimRight(line, " \t")
		}
		if line != "" {
			kept = append(kept, line)
		}
	}
	return html.UnescapeString(strings.Join(kept, " "))
}

// jsString returns s as a JavaScript string literal.
func jsString(s string) string {
	quoted, _ := json.Marshal(s)
	return strings.NewReplacer(`\u0026`, "&", `\u003c`, "<", `\u003e`, ">").Replace(string(quoted))
}

// propertyName returns an attribute name as an object literal key, quoting a
// name that is not an identifier.
func propertyName(name string) string {
	if strings.ContainsAny(name, "-:.") {
		return jsString(name)
	}
	return name
}

// isIntrinsic reports whether an element name is an intrinsic element, which
// JSX writes in lower case, rather than a component.
func isIntrinsic(name string) bool {
	return name[0] >= 'a' && name[0] <= 'z' && !strings.Contains(name, ".")
}

// isSpace reports whether c is whitespace.
func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r'
}

// isWordByte reports whether c may appear in an identifier or a number; bytes
// of non-ASCII characters count, so such identifiers read whole.
func isWordByte(c byte) bool {
	return c == '_' || c == '$' || c >= 0x80 ||
		c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9'
}

// isNameByte reports whether c may appear in an element or attribute name.
func isNameByte(c byte) bool {
	return isWordByte(c) || c == '-' || c == '.' || c == ':'
}
