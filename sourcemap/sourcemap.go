// Package sourcemap reads version 3 source maps, as bundlers write them beside
// a script, to find where a place in the bundle came from.
package sourcemap

import (
	"encoding/json"
	"sort"
	"strings"

	"github.com/pkg/errors"
)

// Map is a decoded source map.
type Map struct {
	// sources names each source file, with the map's source root applied.
	sources []string
	// lines holds the mapped segments of each generated line, by column.
	lines [][]segment
}

// segment maps a generated column to a place in a source, all from zero.
type segment struct {
	column, source, line, sourceColumn int
}

// Position is a place in a source file, line and column from 1.
type Position struct {
	// Source is the file as the map names it, relative to the map's
	// directory unless the map named it absolute.
	Source       string
	Line, Column int
}

// Parse decodes a source map. It skips a segment that names no source, and
// rejects a map whose mappings are not well formed.
func Parse(data []byte) (*Map, error) {
	var raw struct {
		Version    int      `json:"version"`
		SourceRoot string   `json:"sourceRoot"`
		Sources    []string `json:"sources"`
		Mappings   string   `json:"mappings"`
	}
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, errors.Wrap(err, "read the source map")
	}
	if raw.Version != 3 {
		return nil, errors.Errorf("source map version %d, want 3", raw.Version)
	}
	m := &Map{sources: raw.Sources}
	if raw.SourceRoot != "" {
		root := strings.TrimSuffix(raw.SourceRoot, "/") + "/"
		for i, source := range m.sources {
			m.sources[i] = root + source
		}
	}

	// Each line's segments are separated by commas and the lines by
	// semicolons. A segment's fields add to the previous segment's; the
	// generated column starts again at zero on each line.
	var source, line, sourceColumn int
	for _, text := range strings.Split(raw.Mappings, ";") {
		var segments []segment
		column := 0
		for _, field := range strings.Split(text, ",") {
			if field == "" {
				continue
			}
			values, err := decodeVLQ(field)
			if err != nil {
				return nil, err
			}
			column += values[0]
			if len(values) < 4 {
				continue
			}
			source += values[1]
			line += values[2]
			sourceColumn += values[3]
			if source < 0 || source >= len(m.sources) {
				return nil, errors.Errorf("a mapping names source %d of %d", source, len(m.sources))
			}
			segments = append(segments, segment{column: column, source: source, line: line, sourceColumn: sourceColumn})
		}
		sort.SliceStable(segments, func(i, j int) bool { return segments[i].column < segments[j].column })
		m.lines = append(m.lines, segments)
	}
	return m, nil
}

// Locate returns where the generated line and column, from 1, came from: the
// last segment at or before the column. A column of 0 takes the line's first
// segment. It returns false for a line with no segment there.
func (m *Map) Locate(line, column int) (Position, bool) {
	if line < 1 || line > len(m.lines) || len(m.lines[line-1]) == 0 {
		return Position{}, false
	}
	segments := m.lines[line-1]
	found := segments[0]
	if column > 0 {
		at := sort.Search(len(segments), func(i int) bool { return segments[i].column > column-1 })
		if at == 0 {
			return Position{}, false
		}
		found = segments[at-1]
	}
	return Position{Source: m.sources[found.source], Line: found.line + 1, Column: found.sourceColumn + 1}, true
}

// base64Digits are the digits of a VLQ field, in value order.
const base64Digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

// decodeVLQ decodes a segment's fields. Each base64 digit holds five bits,
// lowest first, and a sixth that continues the value; a value's lowest bit
// is its sign.
func decodeVLQ(field string) ([]int, error) {
	var values []int
	value, shift := 0, 0
	for i := 0; i < len(field); i++ {
		digit := strings.IndexByte(base64Digits, field[i])
		if digit < 0 {
			return nil, errors.Errorf("source map mapping %q has a character outside base64", field)
		}
		value |= (digit & 31) << shift
		if digit&32 != 0 {
			shift += 5
			if shift > 30 {
				return nil, errors.Errorf("source map mapping %q holds a value too large", field)
			}
			continue
		}
		if value&1 != 0 {
			value = -(value >> 1)
		} else {
			value >>= 1
		}
		values = append(values, value)
		value, shift = 0, 0
	}
	if shift != 0 {
		return nil, errors.Errorf("source map mapping %q ends inside a value", field)
	}
	return values, nil
}
