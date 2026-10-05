package sandbox

import (
	"bytes"
	"io"
	"strings"
)

// lines passes each complete line written to it to write, as a mod's
// standard output becomes log lines.
type lines struct {
	// write receives each line without its line ending.
	write func(line string)
	// partial holds the text after the last newline.
	partial []byte
}

// Write passes on the complete lines in data and keeps the rest.
func (l *lines) Write(data []byte) (int, error) {
	l.partial = append(l.partial, data...)
	for {
		end := bytes.IndexByte(l.partial, '\n')
		if end < 0 {
			return len(data), nil
		}
		l.write(strings.TrimSuffix(string(l.partial[:end]), "\r"))
		l.partial = l.partial[end+1:]
	}
}

var _ io.Writer = (*lines)(nil)
