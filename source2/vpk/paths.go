// Package vpk reads and writes Valve archives (VPK), the packages Source 2
// games mount their resources from.
package vpk

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"io"
	"path"
	"strings"

	"github.com/pkg/errors"
)

// signature opens every Valve archive directory.
const signature = 0x55aa1234

// embedded is the archive index of a file stored in the directory archive
// itself, after the directory tree.
const embedded = 0x7fff

// entry is one file's directory record.
type entry struct {
	// name is the file's cleaned, lowercase path.
	name string
	// preload are the bytes stored in the directory record.
	preload []byte
	// archive is the index of the archive holding the rest of the file.
	archive uint16
	// offset is where the rest starts in that archive.
	offset uint32
	// length is how many bytes the rest spans.
	length uint32
}

// Paths reads only a Valve archive's directory, including split-archive
// indexes. Payloads are skipped; compatibility checks need resource names
// only.
func Paths(source io.Reader) (map[string]struct{}, error) {
	files := make(map[string]struct{})
	_, err := walk(source, func(file entry) {
		files[file.name] = struct{}{}
	})
	if err != nil {
		return nil, err
	}
	return files, nil
}

// Files reads every file of a single-file Valve archive. A file without
// preload bytes aliases archive. An archive split across numbered parts is
// refused.
func Files(archive []byte) (map[string][]byte, error) {
	// Read the directory and the offset of the embedded data after it.
	var entries []entry
	dataStart, err := walk(bytes.NewReader(archive), func(file entry) {
		entries = append(entries, file)
	})
	if err != nil {
		return nil, err
	}

	// An archive stores each file's bytes once, so its records claim no more
	// bytes than it holds. Records that share data would make the joins below
	// copy it once each.
	var claimed int64
	for _, file := range entries {
		claimed += int64(len(file.preload)) + int64(file.length)
	}
	if claimed > int64(len(archive)) {
		return nil, errors.New("VPK records claim more bytes than the archive holds")
	}

	// Join each file's preload bytes and its embedded data.
	files := make(map[string][]byte, len(entries))
	for _, file := range entries {
		if file.length == 0 {
			files[file.name] = file.preload
			continue
		}
		if file.archive != embedded {
			return nil, errors.Errorf("%s is stored in a separate archive part", file.name)
		}
		start := dataStart + int64(file.offset)
		end := start + int64(file.length)
		if end > int64(len(archive)) {
			return nil, errors.Errorf("%s runs past the end of the archive", file.name)
		}
		if len(file.preload) == 0 {
			files[file.name] = archive[start:end]
			continue
		}
		files[file.name] = append(file.preload, archive[start:end]...)
	}
	return files, nil
}

// walk reads a Valve archive's directory, passing each file's record to
// visit, and returns where the directory archive's embedded data starts.
func walk(source io.Reader, visit func(entry)) (int64, error) {
	// Read the header; version 2 adds four section sizes.
	var header [12]byte
	if _, err := io.ReadFull(source, header[:]); err != nil {
		return 0, err
	}
	if binary.LittleEndian.Uint32(header[:4]) != signature {
		return 0, errors.New("invalid VPK signature")
	}
	headerSize := int64(len(header))
	switch binary.LittleEndian.Uint32(header[4:8]) {
	case 1:
	case 2:
		if _, err := io.CopyN(io.Discard, source, 16); err != nil {
			return 0, err
		}
		headerSize += 16
	default:
		return 0, errors.New("unsupported VPK version")
	}

	// A bounded tree avoids reading multi-gigabyte game or mod payloads.
	size := binary.LittleEndian.Uint32(header[8:])
	if size == 0 || size > 64<<20 {
		return 0, errors.New("VPK directory exceeds the supported 64 MiB limit")
	}
	tree := bufio.NewReader(io.LimitReader(source, int64(size)))
	readString := func() (string, error) {
		value, err := tree.ReadString(0)
		if err != nil {
			return "", err
		}
		return strings.TrimSuffix(value, "\x00"), nil
	}

	// Directory records are grouped by extension, directory and filename.
	for {
		extension, err := readString()
		if err != nil {
			return 0, err
		}
		if extension == "" {
			return headerSize + int64(size), nil
		}
		for {
			directory, err := readString()
			if err != nil {
				return 0, err
			}
			if directory == "" {
				break
			}
			for {
				name, err := readString()
				if err != nil {
					return 0, err
				}
				if name == "" {
					break
				}

				// Read the record and its preload bytes.
				var record [18]byte
				if _, err := io.ReadFull(tree, record[:]); err != nil {
					return 0, err
				}
				if binary.LittleEndian.Uint16(record[16:]) != 0xffff {
					return 0, errors.New("invalid VPK directory record")
				}
				preload := make([]byte, binary.LittleEndian.Uint16(record[4:6]))
				if _, err := io.ReadFull(tree, preload); err != nil {
					return 0, err
				}

				// Name the file by its cleaned path inside the content
				// directory.
				if extension != " " {
					name += "." + extension
				}
				if directory != " " {
					name = directory + "/" + name
				}
				name = path.Clean(strings.ToLower(strings.ReplaceAll(name, `\`, "/")))
				if name == ".." || strings.HasPrefix(name, "../") || path.IsAbs(name) {
					return 0, errors.New("VPK resource name escapes its content directory")
				}
				visit(entry{
					name:    name,
					preload: preload,
					archive: binary.LittleEndian.Uint16(record[6:8]),
					offset:  binary.LittleEndian.Uint32(record[8:12]),
					length:  binary.LittleEndian.Uint32(record[12:16]),
				})
			}
		}
	}
}
