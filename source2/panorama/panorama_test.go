package panorama

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io/fs"
	"reflect"
	"strings"
	"testing"
	"testing/fstest"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
)

// source is a layout using every kind of node the compiler writes.
const source = `<?xml version="1.0" encoding="utf-8"?>
<!-- A menu with its styles, scripts and snippets. -->
<root>
	<styles>
		<include src="s2r://panorama/styles/citadel_base_styles.vcss_c" />
	</styles>
	<scripts>
		<include src="file://{resources}/scripts/menu.js" />
		<script><![CDATA[$.Msg("a < b && c");]]></script>
	</scripts>
	<snippets>
		<snippet name="Row">
			<Label class="Row" text="#row_title" />
		</snippet>
	</snippets>
	<Panel id="Menu" style="width: 100%;" hittest="false">
		<Label text="Tom &amp; Jerry&nbsp;&quot;2&quot;" />
		<Image src="file://{resources}/images/icon.png" />
		<Panel />
	</Panel>
</root>
`

// TestLayout compiles a layout, decompiles it, and compiles the result to the
// same tree; and compiles a script to a resource holding its text.
func TestLayout(t *testing.T) {
	// Compile the layout and read its tree.
	compiled, err := Compile("mymod", "panorama/layout/menu.xml", []byte(source), nil)
	if err != nil {
		t.Fatal(err)
	}
	first := layoutTree(t, compiled)
	root := first["m_AST"].(map[string]any)["m_pRoot"].(map[string]any)
	if root["eType"] != "ROOT" || len(root["vecChildren"].([]any)) != 4 {
		t.Fatalf("compiled root %v", root)
	}

	// Decompile it and compile the result again.
	text, err := Decompile(compiled)
	if err != nil {
		t.Fatal(err)
	}
	again, err := Compile("mymod", "panorama/layout/menu.xml", []byte(text), nil)
	if err != nil {
		t.Fatal(err)
	}
	if second := layoutTree(t, again); !reflect.DeepEqual(first, second) {
		t.Fatalf("the decompiled layout compiles to another tree:\n%s", text)
	}

	// A script stores its text.
	script := []byte(`$.Msg("hello");`)
	compiled, err = Compile("mymod", "panorama/scripts/menu.js", script, nil)
	if err != nil {
		t.Fatal(err)
	}
	if data, err := resource.ReadBlock(compiled, "DATA"); err != nil || !bytes.Equal(data, script) {
		t.Fatalf("the compiled script holds %q, %v", data, err)
	}

	// A layout that does not close names its error.
	if _, err := Compile("mymod", "panorama/layout/bad.xml", []byte("<root><Panel></root>"), nil); err == nil {
		t.Fatal("compiled a layout that does not close")
	}
}

// layoutTree decodes a compiled layout's tree without source positions.
func layoutTree(t *testing.T, compiled []byte) map[string]any {
	t.Helper()
	data, err := resource.ReadBlock(compiled, "LaCo")
	if err != nil {
		t.Fatal(err)
	}
	tree, err := kv3.Decode(data)
	if err != nil {
		t.Fatal(err)
	}
	var strip func(any)
	strip = func(value any) {
		switch value := value.(type) {
		case map[string]any:
			delete(value, "sourceLineColumn")
			for _, child := range value {
				strip(child)
			}
		case []any:
			for _, child := range value {
				strip(child)
			}
		}
	}
	strip(tree)
	return tree.(map[string]any)
}

// sheet is a style sheet using each construct the minifier treats specially.
const sheet = `/* A board. */
@define Gold: #ffd700;
@import url("file://{resources}/styles/base.css");

.Board, .Board > Label
{
	color: Gold;
	font-family: "Radiance Bold", sans-serif;
	transition: opacity 0.5s ;
}

.Board .Icon { background-image: url('file://{images}/icons/mark.png'); }
.Board .Glyph { background-image: url("s2r://panorama/images/icons/x.vsvg"); }

@keyframes 'pulse'
{
	0% { opacity: 1; }
	100% { opacity: 0.5; }
}
`

// TestStyle compiles a sheet to Valve's minified text with its URLs rewritten,
// decompiles it to the same text, refuses a URL outside the game's files, and
// compiles a sheet in a tree, recording the size and source checksum of each
// image it names, from the addon's files or the game's, as Valve's compiler
// does.
func TestStyle(t *testing.T) {
	// Compile the sheet and read its text.
	compiled, err := Compile("mymod", "panorama/styles/board.css", []byte(sheet), nil)
	if err != nil {
		t.Fatal(err)
	}
	const want = `@define Gold: #ffd700;` +
		`@import url("s2r://panorama/styles/base.vcss");` +
		`.Board,.Board > Label{color: Gold;font-family: "Radiance Bold", sans-serif;transition: opacity 0.5s ;}` +
		`.Board .Icon{background-image: url("s2r://panorama/images/icons/mark_png.vtex");}` +
		`.Board .Glyph{background-image: url("s2r://panorama/images/icons/x.vsvg");}` +
		`@keyframes 'pulse'{0%{opacity: 1;}100%{opacity: 0.5;}}`
	text := styleText(t, compiled)
	if text != want {
		t.Fatalf("compiled text\n%s\nwant\n%s", text, want)
	}
	// With nothing to read the images from, the sheet records none, since
	// the game keeps the first size a sheet gives an image.
	if table := styleTable(t, compiled); len(table) != 0 {
		t.Fatalf("a sheet compiled without its images records %v", table)
	}

	// The decompiled sheet compiles to the same text.
	decompiled, err := DecompileStyle(compiled)
	if err != nil {
		t.Fatal(err)
	}
	again, err := Compile("mymod", "panorama/styles/board.css", []byte(decompiled), nil)
	if err != nil {
		t.Fatal(err)
	}
	if text := styleText(t, again); text != want {
		t.Fatalf("the decompiled sheet compiles to\n%s\nfrom\n%s", text, decompiled)
	}

	// A URL outside the game's files is refused with its line.
	for _, url := range []string{"https://example.com/x.png", "file://C:/x.png", "file://{images}/../../x.png"} {
		bad := ".A\n{\n\tbackground-image: url(\"" + url + "\");\n}\n"
		_, err := Compile("mymod", "panorama/styles/bad.css", []byte(bad), nil)
		if err == nil || !strings.Contains(err.Error(), "line 3") {
			t.Fatalf("compiling url(%s): %v", url, err)
		}
	}

	// A tree compiles its sheets beside its layouts, and gives a sheet in an
	// addon's styles directory a sheet layout that includes it.
	// The texture is the addon's and the vector image the game's; the
	// imported sheet is no image, so the table leaves it out.
	game := func(name string) ([]byte, error) {
		if name != "panorama/images/icons/x.vsvg_c" {
			return nil, fs.ErrNotExist
		}
		return testImage(t, "panorama/images/icons/x.svg", 0x9fa7acae, nil), nil
	}
	tree, err := CompileTree("mymod", fstest.MapFS{
		"panorama/styles/board.css":             {Data: []byte(sheet)},
		"panorama/styles/mymod/board.css":       {Data: []byte(sheet)},
		"panorama/images/icons/mark_png.vtex_c": {Data: testImage(t, "panorama/images/icons/mark.png", 0xbe09869e, []uint16{537, 583})},
	}, game)
	if err != nil {
		t.Fatal(err)
	}
	wantTable := []string{
		"panorama/images/icons/mark_png.vtex 537x583 be09869e",
		"panorama/images/icons/x.vsvg 0x0 9fa7acae",
	}
	for _, name := range []string{"panorama/styles/board.vcss_c", "panorama/styles/mymod/board.vcss_c"} {
		if file := tree[name]; file == nil || styleText(t, file.Data) != want {
			t.Fatalf("the tree holds %v", tree)
		}
		if table := styleTable(t, tree[name].Data); !reflect.DeepEqual(table, wantTable) {
			t.Fatalf("%s records %v, want %v", name, table, wantTable)
		}
	}
	if tree["panorama/layout/board.css.vxml_c"] != nil {
		t.Fatal("a sheet outside an addon's styles directory got a sheet layout")
	}
	wrapper := tree["panorama/layout/mymod/board.css.vxml_c"]
	if wrapper == nil {
		t.Fatalf("the tree holds no sheet layout: %v", tree)
	}
	layout, err := Decompile(wrapper.Data)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(layout, `src="s2r://panorama/styles/mymod/board.vcss"`) {
		t.Fatalf("the sheet layout is\n%s", layout)
	}
}

// styleText returns a compiled sheet's text, after its checksum and image
// table.
func styleText(t *testing.T, compiled []byte) string {
	t.Helper()
	data, at := styleData(t, compiled)
	return string(data[at:])
}

// styleTable returns a compiled sheet's image table, one "name WxH crc" line
// per image.
func styleTable(t *testing.T, compiled []byte) []string {
	t.Helper()
	data, _ := styleData(t, compiled)
	var table []string
	at := 6
	for range binary.LittleEndian.Uint16(data[4:]) {
		end := at + bytes.IndexByte(data[at:], 0)
		entry := data[end+1:]
		table = append(table, fmt.Sprintf("%s %dx%d %08x", data[at:end],
			binary.LittleEndian.Uint16(entry), binary.LittleEndian.Uint16(entry[2:]), binary.LittleEndian.Uint32(entry[4:])))
		at = end + 9
	}
	return table
}

// styleData returns a compiled sheet's data block and the offset of its text.
func styleData(t *testing.T, compiled []byte) ([]byte, int) {
	t.Helper()
	data, err := resource.ReadBlock(compiled, "DATA")
	if err != nil {
		t.Fatal(err)
	}
	at := 6
	for range binary.LittleEndian.Uint16(data[4:]) {
		at += bytes.IndexByte(data[at:], 0) + 9
	}
	return data, at
}

// testImage builds a compiled image whose edit record names source with crc:
// a texture of the given width and height, or a vector image when size is
// nil.
func testImage(t *testing.T, source string, crc uint32, size []uint16) []byte {
	t.Helper()
	red2, err := editInfo([]kv3.Object{dependency("mymod", source, crc, true)}, "CompileTexture", "Texture Compiler Version", 11)
	if err != nil {
		t.Fatal(err)
	}
	data := []byte("<svg/>")
	if size != nil {
		data = binary.LittleEndian.AppendUint16(nil, 1)
		data = append(data, make([]byte, 18)...)
		data = binary.LittleEndian.AppendUint16(data, size[0])
		data = binary.LittleEndian.AppendUint16(data, size[1])
	}
	image, err := resource.Write(1, []resource.Block{{Tag: "RED2", Data: red2}, {Tag: "DATA", Data: data}})
	if err != nil {
		t.Fatal(err)
	}
	return image
}
