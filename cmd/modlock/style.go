package main

import (
	"os"
	"strings"
	"unicode/utf8"

	"golang.org/x/term"
)

// tone is the terminal style a piece of text shows in, as its SGR code.
type tone string

// The tones the command line's text uses.
const (
	bold   tone = "1"
	dim    tone = "2"
	red    tone = "31"
	green  tone = "32"
	yellow tone = "33"
	cyan   tone = "36"
)

// terminal reports whether f is a terminal a person reads.
func terminal(f *os.File) bool {
	return term.IsTerminal(int(f.Fd()))
}

// ansi reports whether f is a terminal that obeys ANSI codes, which move the
// cursor and set color.
func ansi(f *os.File) bool {
	return terminal(f) && os.Getenv("TERM") != "dumb" && enableColor(f)
}

// colorful reports whether text for the terminal f may carry color: it is a
// terminal that shows color, and NO_COLOR does not turn color off.
func colorful(f *os.File) bool {
	return os.Getenv("NO_COLOR") == "" && ansi(f)
}

// paint returns text in tone when on is true, and text unchanged otherwise.
func paint(on bool, t tone, text string) string {
	if !on {
		return text
	}
	return "\x1b[" + string(t) + "m" + text + "\x1b[0m"
}

// frame draws a rounded box around lines, padded to the widest of them as the
// terminal shows them. edge paints the box itself.
func frame(lines []string, edge func(string) string) string {
	// Find the widest line.
	widest := 0
	for _, line := range lines {
		widest = max(widest, visible(line))
	}

	// Draw the top, each line between the sides, and the bottom.
	var out strings.Builder
	bar := strings.Repeat("─", widest+4)
	out.WriteString(edge("╭"+bar+"╮") + "\n")
	for _, line := range lines {
		out.WriteString(edge("│") + "  " + line + strings.Repeat(" ", widest-visible(line)) + "  " + edge("│") + "\n")
	}
	out.WriteString(edge("╰"+bar+"╯") + "\n")
	return out.String()
}

// visible returns how many columns text fills, skipping its color codes.
func visible(text string) int {
	columns := 0
	for escaped := false; text != ""; {
		r, size := utf8.DecodeRuneInString(text)
		text = text[size:]
		switch {
		case r == '\x1b':
			escaped = true
		case escaped && r == 'm':
			escaped = false
		case !escaped:
			columns++
		}
	}
	return columns
}
