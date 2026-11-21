package panorama

import (
	"fmt"
	"strings"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/pkg/errors"
)

// Source returns the source path of a compiled Panorama resource path, such
// as panorama/layout/hud.xml for panorama/layout/hud.vxml_c, and whether
// DecompileFile prints that kind of resource. It inverts Compiled.
func Source(compiled string) (string, bool) {
	for _, ext := range []string{".xml", ".js", ".css"} {
		if suffix := ".v" + ext[1:] + "_c"; strings.HasSuffix(compiled, suffix) {
			return strings.TrimSuffix(compiled, suffix) + ext, true
		}
	}
	return "", false
}

// DecompileFile prints the compiled resource at path as the source Compile
// builds it from: a layout as XML, a style sheet as CSS and a script as its
// stored text.
func DecompileFile(path string, compiled []byte) ([]byte, error) {
	switch {
	case strings.HasSuffix(path, ".vjs_c"):
		return resource.ReadBlock(compiled, "DATA")
	case strings.HasSuffix(path, ".vcss_c"):
		text, err := DecompileStyle(compiled)
		return []byte(text), err
	default:
		text, err := Decompile(compiled)
		return []byte(text), err
	}
}

// Decompile prints a compiled .vxml_c layout as XML that Compile builds back
// to the same tree, apart from source positions. A layout is the only
// Panorama resource that needs it; a compiled script's DATA block is its
// source.
func Decompile(compiled []byte) (string, error) {
	// Read the layout tree.
	tree, err := resource.ReadBlock(compiled, "LaCo")
	if err != nil {
		return "", err
	}
	value, err := kv3.Decode(tree)
	if err != nil {
		return "", err
	}
	top, _ := value.(map[string]any)
	ast, _ := top["m_AST"].(map[string]any)
	root, _ := ast["m_pRoot"].(map[string]any)
	if root == nil {
		return "", errors.New("The layout tree has no root")
	}

	// Print from the root.
	var out strings.Builder
	if err := printNode(&out, root, 0); err != nil {
		return "", err
	}
	return out.String(), nil
}

// printNode writes a node with its attributes, then its children indented.
func printNode(out *strings.Builder, node map[string]any, depth int) error {
	// Split the attributes from the child nodes.
	indent := strings.Repeat("\t", depth)
	kind, _ := node["eType"].(string)
	var attributes strings.Builder
	var children []map[string]any
	for _, child := range nested(node) {
		if child["eType"] == "PANEL_ATTRIBUTE" {
			fmt.Fprintf(&attributes, " %s=\"%s\"", child["name"], escape(target(child["child"])))
			continue
		}
		children = append(children, child)
	}

	// Name the element by the node.
	name := strings.ToLower(kind)
	switch kind {
	case "INCLUDE":
		fmt.Fprintf(out, "%s<include src=\"%s\" />\n", indent, escape(target(node["child"])))
		return nil
	case "SCRIPT_BODY":
		fmt.Fprintf(out, "%s<script><![CDATA[%s]]></script>\n", indent, node["name"])
		return nil
	case "PANEL":
		name, _ = node["name"].(string)
	case "SNIPPET":
		name = fmt.Sprintf("snippet name=\"%s\"", escape(fmt.Sprint(node["name"])))
	case "ROOT", "STYLES", "SCRIPTS", "SNIPPETS":
	default:
		return errors.Errorf("Unknown layout node %q", kind)
	}

	// Print an empty element closed, or its children between its tags.
	if len(children) == 0 {
		fmt.Fprintf(out, "%s<%s%s />\n", indent, name, attributes.String())
		return nil
	}
	fmt.Fprintf(out, "%s<%s%s>\n", indent, name, attributes.String())
	for _, child := range children {
		if err := printNode(out, child, depth+1); err != nil {
			return err
		}
	}
	fmt.Fprintf(out, "%s</%s>\n", indent, strings.Fields(name)[0])
	return nil
}

// nested returns a node's child or children.
func nested(node map[string]any) []map[string]any {
	var out []map[string]any
	if child, ok := node["child"].(map[string]any); ok {
		out = append(out, child)
	}
	list, _ := node["vecChildren"].([]any)
	for _, item := range list {
		if child, ok := item.(map[string]any); ok {
			out = append(out, child)
		}
	}
	return out
}

// target returns the text a reference or attribute value was compiled from.
func target(value any) string {
	node, _ := value.(map[string]any)
	name, _ := node["name"].(string)
	switch node["eType"] {
	case "REFERENCE_COMPILED":
		return "s2r://" + name
	case "REFERENCE_PASSTHROUGH":
		return "file://" + name
	default:
		return name
	}
}

// escape writes the characters XML reserves as character references.
func escape(text string) string {
	return strings.NewReplacer("&", "&#38;", "<", "&#60;", ">", "&#62;", `"`, "&#34;", "'", "&#39;").Replace(text)
}
