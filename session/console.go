package session

import (
	"bytes"
	"io"
)

// consoleNoise starts the line the server's text console writes on every
// frame when its standard input is not a console, as under a controller.
var consoleNoise = []byte("CTextConsoleWin::GetLine:")

// consoleFilter passes the host's console output on without its per-frame
// console noise. It holds a partial line until the line ends.
type consoleFilter struct {
	// out receives the kept lines.
	out io.Writer
	// partial holds the start of a line that has not ended yet.
	partial []byte
}

// Write passes on each complete line of data that is not console noise.
func (f *consoleFilter) Write(data []byte) (int, error) {
	f.partial = append(f.partial, data...)
	for {
		end := bytes.IndexByte(f.partial, '\n')
		if end < 0 {
			return len(data), nil
		}
		line := f.partial[:end+1]
		f.partial = f.partial[end+1:]
		if bytes.HasPrefix(line, consoleNoise) {
			continue
		}
		if _, err := f.out.Write(line); err != nil {
			return len(data), err
		}
	}
}
