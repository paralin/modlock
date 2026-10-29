// Package panorama compiles Panorama layouts, scripts and style sheets into
// the resources the game loads, without Valve's resource compiler, and
// decompiles layouts back to XML and style sheets back to CSS. The output
// follows what the resource compiler writes, so the game reads it the same way
// on every platform.
package panorama

import (
	"encoding/binary"
	"hash/crc32"
	"path"
	"strings"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/pkg/errors"
)

// Compiled returns the path of the resource the game loads for a source
// path, such as panorama/layout/hud.vxml_c for panorama/layout/hud.xml, and
// whether Compile builds that kind of source.
func Compiled(source string) (string, bool) {
	switch path.Ext(source) {
	case ".xml", ".js", ".css":
		return compiledName(source) + "_c", true
	default:
		return "", false
	}
}

// Compile builds the source at path, such as panorama/layout/hud.xml,
// panorama/scripts/hud.js or panorama/styles/hud.css inside the addon named
// addon, into its compiled resource. A layout is parsed into the tree the game
// reads, a style sheet is minified with its paths rewritten, and a script is
// stored as it is.
func Compile(addon, path string, source []byte) ([]byte, error) {
	switch {
	case strings.HasSuffix(path, ".xml"):
		return layout(addon, path, source)
	case strings.HasSuffix(path, ".js"):
		return script(addon, path, source)
	case strings.HasSuffix(path, ".css"):
		return style(addon, path, source)
	default:
		return nil, errors.Errorf("%s is not a Panorama layout, script or style sheet", path)
	}
}

// script compiles a script to its .vjs_c resource, which stores the text.
func script(addon, path string, source []byte) ([]byte, error) {
	red2, err := editInfo([]kv3.Object{
		dependency(addon, path, crc32.ChecksumIEEE(source), true),
		dependency(addon, strings.TrimSuffix(path, ".js")+".vjs", 0, false),
	}, "CompileJavaScript", "JavaScript Compiler Version", 4)
	if err != nil {
		return nil, err
	}
	return resource.Write(4, []resource.Block{{Tag: "RED2", Data: red2}, {Tag: "DATA", Data: source}})
}

// layout compiles a layout to its .vxml_c resource, which stores the parsed
// layout tree.
func layout(addon, path string, source []byte) ([]byte, error) {
	// Parse the tree first, so a malformed layout names its error.
	root, err := parse(source)
	if err != nil {
		return nil, errors.Wrap(err, path)
	}
	tree, err := kv3.Encode(kv3.Object{{Key: "m_AST", Value: kv3.Object{{Key: "m_pRoot", Value: root.node()}}}})
	if err != nil {
		return nil, err
	}

	// Record the source's checksum in the edit record and the data block.
	crc := crc32.ChecksumIEEE(source)
	red2, err := editInfo([]kv3.Object{
		dependency(addon, strings.TrimSuffix(path, ".xml")+".vxml", 0, false),
		dependency(addon, path, crc, true),
	}, "CompilePanorama", "Panorama Layout Compiler Version", 14)
	if err != nil {
		return nil, err
	}
	data := binary.LittleEndian.AppendUint32(nil, crc)
	data = binary.LittleEndian.AppendUint16(data, 0)
	return resource.Write(3, []resource.Block{{Tag: "RED2", Data: red2}, {Tag: "DATA", Data: data}, {Tag: "LaCo", Data: tree}})
}

// dependency is one input file in a resource's edit record.
func dependency(addon, path string, crc uint32, exists bool) kv3.Object {
	return kv3.Object{
		{Key: "m_RelativeFilename", Value: path},
		{Key: "m_SearchPath", Value: "citadel_addons/" + addon},
		{Key: "m_nFileCRC", Value: crc},
		{Key: "m_bOptional", Value: !exists},
		{Key: "m_bFileExists", Value: exists},
		{Key: "m_bIsGameFile", Value: false},
	}
}

// editInfo encodes the RED2 block recording what a compiled resource was
// built from, by which compiler.
func editInfo(inputs []kv3.Object, compiler, name string, version int) ([]byte, error) {
	special := func(text string, fingerprint int) kv3.Object {
		return kv3.Object{
			{Key: "m_String", Value: text},
			{Key: "m_CompilerIdentifier", Value: compiler},
			{Key: "m_nFingerprint", Value: fingerprint},
			{Key: "m_nUserData", Value: 0},
		}
	}
	dependencies := make([]any, len(inputs))
	for i, input := range inputs {
		dependencies[i] = input
	}
	return kv3.Encode(kv3.Object{
		{Key: "m_InputDependencies", Value: dependencies},
		{Key: "m_AdditionalInputDependencies", Value: []any{}},
		{Key: "m_ArgumentDependencies", Value: []any{kv3.Object{
			{Key: "m_ParameterName", Value: "___OverrideInputData___"},
			{Key: "m_ParameterType", Value: "BinaryBlobArg"},
			{Key: "m_nFingerprint", Value: 0},
			{Key: "m_nFingerprintDefault", Value: 0},
		}}},
		{Key: "m_SpecialDependencies", Value: []any{
			special(name, version),
			special("Panorama Preprocessor Presence/Version", 0),
		}},
		{Key: "m_SpecialInputDependencies", Value: []any{}},
		{Key: "m_AdditionalRelatedFiles", Value: []any{}},
		{Key: "m_ChildResourceList", Value: []any{}},
		{Key: "m_WeakReferenceList", Value: []any{}},
		{Key: "m_SearchableUserData", Value: kv3.Object{{Key: "IsChildResource", Value: 0}}},
		{Key: "m_SubassetReferences", Value: nil},
		{Key: "m_SubassetDefinitions", Value: nil},
	})
}
