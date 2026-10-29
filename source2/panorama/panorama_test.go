package panorama

import (
	"bytes"
	"encoding/binary"
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
	compiled, err := Compile("mymod", "panorama/layout/menu.xml", []byte(source))
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
	again, err := Compile("mymod", "panorama/layout/menu.xml", []byte(text))
	if err != nil {
		t.Fatal(err)
	}
	if second := layoutTree(t, again); !reflect.DeepEqual(first, second) {
		t.Fatalf("the decompiled layout compiles to another tree:\n%s", text)
	}

	// A script stores its text.
	script := []byte(`$.Msg("hello");`)
	compiled, err = Compile("mymod", "panorama/scripts/menu.js", script)
	if err != nil {
		t.Fatal(err)
	}
	if data, err := resource.ReadBlock(compiled, "DATA"); err != nil || !bytes.Equal(data, script) {
		t.Fatalf("the compiled script holds %q, %v", data, err)
	}

	// A layout that does not close names its error.
	if _, err := Compile("mymod", "panorama/layout/bad.xml", []byte("<root><Panel></root>")); err == nil {
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
// compiles a sheet in a tree.
func TestStyle(t *testing.T) {
	// Compile the sheet and read its text.
	compiled, err := Compile("mymod", "panorama/styles/board.css", []byte(sheet))
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

	// The decompiled sheet compiles to the same text.
	decompiled, err := DecompileStyle(compiled)
	if err != nil {
		t.Fatal(err)
	}
	again, err := Compile("mymod", "panorama/styles/board.css", []byte(decompiled))
	if err != nil {
		t.Fatal(err)
	}
	if text := styleText(t, again); text != want {
		t.Fatalf("the decompiled sheet compiles to\n%s\nfrom\n%s", text, decompiled)
	}

	// A URL outside the game's files is refused with its line.
	for _, url := range []string{"https://example.com/x.png", "file://C:/x.png", "file://{images}/../../x.png"} {
		bad := ".A\n{\n\tbackground-image: url(\"" + url + "\");\n}\n"
		_, err := Compile("mymod", "panorama/styles/bad.css", []byte(bad))
		if err == nil || !strings.Contains(err.Error(), "line 3") {
			t.Fatalf("compiling url(%s): %v", url, err)
		}
	}

	// A tree compiles its sheets beside its layouts.
	tree, err := CompileTree("mymod", fstest.MapFS{"panorama/styles/board.css": {Data: []byte(sheet)}})
	if err != nil {
		t.Fatal(err)
	}
	if file := tree["panorama/styles/board.vcss_c"]; file == nil || styleText(t, file.Data) != want {
		t.Fatalf("the tree holds %v", tree)
	}
}

// styleText returns a compiled sheet's text, after its checksum and resource
// table.
func styleText(t *testing.T, compiled []byte) string {
	t.Helper()
	data, err := resource.ReadBlock(compiled, "DATA")
	if err != nil {
		t.Fatal(err)
	}
	at := 6
	for range binary.LittleEndian.Uint16(data[4:]) {
		at += bytes.IndexByte(data[at:], 0) + 9
	}
	return string(data[at:])
}
