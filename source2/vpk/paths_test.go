package vpk

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// TestFilesRefusesSharedData reads a record with preload bytes, and refuses
// an archive whose records all point at the same embedded data, which Files
// would copy once per record.
func TestFilesRefusesSharedData(t *testing.T) {
	// One record reads as its preload byte and the data.
	data := bytes.Repeat([]byte{1}, 4096)
	files, err := Files(sharedDataArchive(data, "a"))
	if err != nil || len(files["a.bin"]) != 1+len(data) {
		t.Fatal("a record with preload bytes did not read", err)
	}

	// Three records that share the data are refused.
	if _, err := Files(sharedDataArchive(data, "a", "b", "c")); err == nil {
		t.Fatal("records sharing embedded data were joined")
	}
}

// sharedDataArchive packs a version 1 archive whose named records each hold
// one preload byte and claim all of data.
func sharedDataArchive(data []byte, names ...string) []byte {
	// Write the directory: one extension, the root directory, the records.
	var tree bytes.Buffer
	tree.WriteString("bin\x00 \x00")
	for _, name := range names {
		tree.WriteString(name + "\x00")
		var record [18]byte
		binary.LittleEndian.PutUint16(record[4:], 1)
		binary.LittleEndian.PutUint16(record[6:], embedded)
		binary.LittleEndian.PutUint32(record[12:], uint32(len(data)))
		binary.LittleEndian.PutUint16(record[16:], 0xffff)
		tree.Write(record[:])
		tree.WriteByte('p')
	}
	tree.WriteString("\x00\x00\x00")

	// Pack the header, the directory and the data.
	archive := binary.LittleEndian.AppendUint32(nil, signature)
	archive = binary.LittleEndian.AppendUint32(archive, 1)
	archive = binary.LittleEndian.AppendUint32(archive, uint32(tree.Len()))
	return append(append(archive, tree.Bytes()...), data...)
}
