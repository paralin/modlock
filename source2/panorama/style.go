package panorama

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"path"
	"slices"
	"strings"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/pkg/errors"
)

// style compiles a style sheet to its .vcss_c resource: the minified text
// with each url() rewritten to the compiled resource the game loads, a table
// of those resources, and a source map from the text back to the sheet's
// lines.
func style(addon, file string, source []byte) ([]byte, error) {
	sheet, err := minify(source)
	if err != nil {
		return nil, errors.Wrap(err, file)
	}

	// The data block holds the source's checksum, then each referenced
	// resource with an image's width and height and a hash this compiler
	// leaves zero, then the text.
	crc := crc32.ChecksumIEEE(source)
	data := binary.LittleEndian.AppendUint32(nil, crc)
	data = binary.LittleEndian.AppendUint16(data, uint16(len(sheet.references)))
	for _, reference := range sheet.references {
		data = append(append(data, reference...), 0)
		data = append(data, make([]byte, 8)...)
	}
	data = append(data, sheet.text...)

	// The source map holds, for each selector and declaration, its offset in
	// the text and its line and column in the source.
	var spans any
	if len(sheet.spans) != 0 {
		list := make([]any, len(sheet.spans))
		for i, span := range sheet.spans {
			list[i] = []any{span[0], span[1], span[2]}
		}
		spans = list
	}
	sourceMap, err := kv3.Encode(kv3.Object{{Key: "DBITSLC", Value: spans}})
	if err != nil {
		return nil, err
	}

	red2, err := editInfo([]kv3.Object{
		dependency(addon, file, crc, true),
		dependency(addon, strings.TrimSuffix(file, ".css")+".vcss", 0, false),
	}, "CompilePanorama", "Panorama Style Compiler Version", 11)
	if err != nil {
		return nil, err
	}
	return resource.Write(3, []resource.Block{{Tag: "RED2", Data: red2}, {Tag: "DATA", Data: data}, {Tag: "SrMa", Data: sourceMap}})
}

// compiledSheet is a style sheet minified as Valve's resource compiler
// writes it.
type compiledSheet struct {
	// text is the minified sheet.
	text []byte
	// references are the resources its url()s name, without the s2r://
	// scheme, in order of first use.
	references []string
	// spans are the offset in text, the 1-based line and the 0-based column
	// in the source of each selector and declaration.
	spans [][3]int
}

// minify removes a sheet's comments and the whitespace between its rules and
// declarations, collapses other whitespace outside strings to one space, and
// rewrites each url() to the compiled resource. A rule or declaration keeps
// its text otherwise, so the game reads what the author wrote.
func minify(source []byte) (*compiledSheet, error) {
	m := &minifier{source: source, line: 1}
	if err := m.block(0); err != nil {
		return nil, err
	}
	return &m.sheet, nil
}

// minifier reads a sheet from the front, tracking its position.
type minifier struct {
	source []byte
	// at is the offset of the next byte to read; line and column give its
	// position, line 1-based and column 0-based.
	at, line, column int
	sheet            compiledSheet
}

// syntaxError returns an error at a line of the source.
func (m *minifier) syntaxError(line int, format string, args ...any) error {
	return errors.Errorf("line %d: "+format, append([]any{line}, args...)...)
}

// advance moves past n bytes.
func (m *minifier) advance(n int) {
	for range n {
		if m.source[m.at] == '\n' {
			m.line, m.column = m.line+1, 0
		} else {
			m.column++
		}
		m.at++
	}
}

// skipSpace moves past whitespace and comments.
func (m *minifier) skipSpace() error {
	for m.at < len(m.source) {
		switch {
		case isSpace(m.source[m.at]):
			m.advance(1)
		case strings.HasPrefix(string(m.source[m.at:min(m.at+2, len(m.source))]), "/*"):
			line := m.line
			end := strings.Index(string(m.source[m.at+2:]), "*/")
			if end < 0 {
				return m.syntaxError(line, "the comment is not closed")
			}
			m.advance(end + 4)
		default:
			return nil
		}
	}
	return nil
}

// block reads rules and declarations up to the end of the sheet at depth 0,
// or up to the closing brace of a block, which it leaves to the caller.
func (m *minifier) block(depth int) error {
	for {
		if err := m.skipSpace(); err != nil {
			return err
		}
		if m.at == len(m.source) {
			if depth != 0 {
				return m.syntaxError(m.line, "a block is not closed with }")
			}
			return nil
		}
		if m.source[m.at] == '}' {
			if depth == 0 {
				return m.syntaxError(m.line, "} closes no block")
			}
			return nil
		}

		// Read one item up to the brace or semicolon that ends it.
		line, column := m.line, m.column
		item, end, err := m.item()
		if err != nil {
			return err
		}
		m.sheet.spans = append(m.sheet.spans, [3]int{len(m.sheet.text), line, column})
		switch {
		case end == '{':
			m.sheet.text = append(m.sheet.text, selector(item)...)
			m.sheet.text = append(m.sheet.text, '{')
			m.advance(1)
			if err := m.block(depth + 1); err != nil {
				return err
			}
			m.sheet.text = append(m.sheet.text, '}')
			m.advance(1)
		case depth == 0 && !strings.HasPrefix(item, "@"):
			return m.syntaxError(line, "%q is outside a rule", item)
		default:
			rewritten, err := m.urls(line, item)
			if err != nil {
				return err
			}
			m.sheet.text = append(append(m.sheet.text, rewritten...), ';')
			if end == ';' {
				m.advance(1)
			}
		}
	}
}

// item reads the text up to the next {, ; or } outside strings, with
// comments removed and whitespace collapsed, and returns it
// with the byte that ends it, which it does not consume, or 0 at the end of
// the sheet.
func (m *minifier) item() (string, byte, error) {
	var out []byte
	space, line := false, m.line
	for m.at < len(m.source) {
		c := m.source[m.at]
		switch {
		case c == '"' || c == '\'':
			// A string keeps its text.
			end := m.at + 1
			for end < len(m.source) && m.source[end] != c && m.source[end] != '\n' {
				if m.source[end] == '\\' {
					end++
				}
				end++
			}
			if end >= len(m.source) || m.source[end] != c {
				return "", 0, m.syntaxError(m.line, "the string is not closed")
			}
			if space && len(out) != 0 {
				out = append(out, ' ')
			}
			out, space = append(out, m.source[m.at:end+1]...), false
			m.advance(end + 1 - m.at)
			continue
		case isSpace(c) || strings.HasPrefix(string(m.source[m.at:min(m.at+2, len(m.source))]), "/*"):
			if err := m.skipSpace(); err != nil {
				return "", 0, err
			}
			space = true
			continue
		case c == '{' || c == ';' || c == '}':
			// Space before a semicolon stays, as Valve's compiler keeps it.
			if c == ';' && space && len(out) != 0 {
				out = append(out, ' ')
			}
			return string(out), c, nil
		}
		if space && len(out) != 0 {
			out = append(out, ' ')
		}
		out, space = append(out, c), false
		m.advance(1)
	}
	if len(out) == 0 {
		return "", 0, m.syntaxError(line, "the sheet ends early")
	}
	return string(out), 0, nil
}

// selector returns a rule's selector without the spaces around the commas of
// a selector list.
func selector(text string) string {
	parts := strings.Split(text, ",")
	for i, part := range parts {
		parts[i] = strings.TrimSpace(part)
	}
	return strings.Join(parts, ",")
}

// urls rewrites each url() in a declaration or @import to the compiled
// resource the game loads, and adds it to the sheet's references.
func (m *minifier) urls(line int, text string) (string, error) {
	var out strings.Builder
	for {
		start := strings.Index(text, "url(")
		if start < 0 {
			out.WriteString(text)
			return out.String(), nil
		}
		end := strings.IndexByte(text[start:], ')')
		if end < 0 {
			return "", m.syntaxError(line, "url( is not closed")
		}
		end += start
		target := strings.Trim(strings.TrimSpace(text[start+4:end]), `"'`)
		compiled, err := compiledURL(target)
		if err != nil {
			return "", m.syntaxError(line, "%s", err)
		}
		out.WriteString(text[:start])
		out.WriteString(`url("` + compiled + `")`)
		if name, ok := strings.CutPrefix(compiled, "s2r://"); ok && !slices.Contains(m.sheet.references, name) {
			m.sheet.references = append(m.sheet.references, name)
		}
		text = text[end+1:]
	}
}

// compiledURL returns the URL the game loads for a sheet's url(): a compiled
// resource as s2r://, or another panel as panel://. A source file under
// {images} or {resources} names the resource it compiles to, as an image
// file://{images}/icons/x.png names s2r://panorama/images/icons/x_png.vtex. A
// sheet reads only the game's files, never the network or the disk.
func compiledURL(target string) (string, error) {
	scheme, rest, _ := strings.Cut(target, "://")
	if slices.Contains(strings.Split(rest, "/"), "..") {
		return "", errors.Errorf("%s leaves the game's files", target)
	}
	switch scheme {
	case "s2r", "panel":
		return target, nil
	case "file":
		var base string
		switch {
		case strings.HasPrefix(rest, "{images}/"):
			base = "panorama/images/" + strings.TrimPrefix(rest, "{images}/")
		case strings.HasPrefix(rest, "{resources}/"):
			base = "panorama/" + strings.TrimPrefix(rest, "{resources}/")
		default:
			return "", errors.Errorf("%s is outside {images} and {resources}", target)
		}
		return "s2r://" + compiledName(base), nil
	default:
		return "", errors.Errorf("%s is not a game file; use s2r://, file://{images}/ or file://{resources}/", target)
	}
}

// compiledName returns the name the game gives the resource a Panorama source
// compiles to, without the _c every compiled file ends with: a layout x.xml
// to x.vxml, a script x.js to x.vjs, a sheet x.css to x.vcss, a vector image
// x.svg to x.vsvg, and a raster image x.png to x_png.vtex. Any other name is
// kept.
func compiledName(name string) string {
	ext := path.Ext(name)
	base := strings.TrimSuffix(name, ext)
	switch strings.ToLower(ext) {
	case ".xml", ".js", ".css", ".svg":
		return base + ".v" + strings.ToLower(ext[1:])
	case ".png", ".jpg", ".jpeg", ".tga", ".psd":
		return base + "_" + strings.ToLower(ext[1:]) + ".vtex"
	default:
		return name
	}
}

// isSpace reports whether c is CSS whitespace.
func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
}

// DecompileStyle prints a compiled .vcss_c style sheet with one declaration
// per line, which Compile builds back to the same text.
func DecompileStyle(compiled []byte) (string, error) {
	// Skip the checksum and the table of referenced resources.
	data, err := resource.ReadBlock(compiled, "DATA")
	if err != nil {
		return "", err
	}
	if len(data) < 6 {
		return "", errors.New("The style sheet's data block is too short")
	}
	at := 6
	for range binary.LittleEndian.Uint16(data[4:]) {
		end := bytes.IndexByte(data[at:], 0)
		if end < 0 || at+end+9 > len(data) {
			return "", errors.New("The style sheet's resource table is cut short")
		}
		at += end + 9
	}

	// Break the text after each brace and semicolon outside strings.
	var out strings.Builder
	text, depth, line := data[at:], 0, true
	indent := func() {
		if line {
			out.WriteString(strings.Repeat("\t", depth))
			line = false
		}
	}
	for i := 0; i < len(text); i++ {
		switch c := text[i]; c {
		case '"', '\'':
			end := i + 1 + bytes.IndexByte(text[i+1:], c)
			if end == i {
				end = len(text) - 1
			}
			indent()
			out.Write(text[i : end+1])
			i = end
		case '{':
			out.WriteString("\n")
			line = true
			indent()
			out.WriteString("{\n")
			depth++
			line = true
		case ';':
			out.WriteString(";\n")
			line = true
		case '}':
			depth = max(depth-1, 0)
			line = true
			indent()
			out.WriteString("}\n")
			if depth == 0 {
				out.WriteString("\n")
			}
			line = true
		default:
			indent()
			out.WriteByte(c)
		}
	}
	return out.String(), nil
}
