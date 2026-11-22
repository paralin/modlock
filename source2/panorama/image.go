package panorama

import (
	"encoding/binary"
	"strings"

	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/pkg/errors"
)

// imageEntry is what a style sheet records about an image it names: the
// image's size and the checksum of its source file. The game takes the size
// as the image's own wherever the image is drawn, the game's own panels
// included, when the sheet is the first to name it, so an entry with the
// wrong size hides the image everywhere until the game restarts.
type imageEntry struct {
	width, height uint16
	crc           uint32
}

// isImage reports whether a compiled resource name, such as
// panorama/images/x_png.vtex, is an image a sheet's table lists.
func isImage(name string) bool {
	return strings.HasSuffix(name, ".vtex") || strings.HasSuffix(name, ".vsvg")
}

// findImage returns the entry for the image name, reading its compiled
// resource with read, and whether read found and parsed it. A nil read finds
// nothing.
func findImage(read func(name string) ([]byte, error), name string) (imageEntry, bool) {
	// Read the image's compiled resource, and describe it.
	if read == nil {
		return imageEntry{}, false
	}
	compiled, err := read(name + "_c")
	if err != nil {
		return imageEntry{}, false
	}
	entry, err := describeImage(name, compiled)
	return entry, err == nil
}

// describeImage reads the entry for the image name from its compiled resource:
// a texture's size from its header, zero for a vector image, and the checksum
// its edit record gives its source file. Valve's compiler writes the same entry
// for every image the game's own sheets name.
func describeImage(name string, compiled []byte) (imageEntry, error) {
	// Split the resource into its blocks.
	blocks, err := resource.Read(compiled)
	if err != nil {
		return imageEntry{}, err
	}
	var entry imageEntry

	// A texture's data block opens with its version, flags and four floats
	// of reflectivity, then its width and height.
	if strings.HasSuffix(name, ".vtex") {
		data, _ := resource.Find(blocks, "DATA")
		if len(data) < 24 || binary.LittleEndian.Uint16(data) != 1 {
			return imageEntry{}, errors.Errorf("%s has no texture header this compiler reads", name)
		}
		entry.width = binary.LittleEndian.Uint16(data[20:])
		entry.height = binary.LittleEndian.Uint16(data[22:])
	}

	// The source is the first input the edit record lists as present: a
	// vector image's .svg, or the .png a texture was built from.
	red2, ok := resource.Find(blocks, "RED2")
	if !ok {
		return imageEntry{}, errors.Errorf("%s has no edit record", name)
	}
	decoded, err := kv3.Decode(red2)
	if err != nil {
		return imageEntry{}, errors.Wrap(err, name)
	}
	record, _ := decoded.(map[string]any)
	for _, key := range []string{"m_InputDependencies", "m_AdditionalInputDependencies"} {
		inputs, _ := record[key].([]any)
		for _, input := range inputs {
			input, _ := input.(map[string]any)
			if input["m_bFileExists"] != true {
				continue
			}
			switch crc := input["m_nFileCRC"].(type) {
			case int64:
				entry.crc = uint32(crc)
			case uint64:
				entry.crc = uint32(crc)
			}
			return entry, nil
		}
	}
	return imageEntry{}, errors.Errorf("%s names no source file", name)
}
