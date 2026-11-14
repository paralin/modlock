// Package prompt asks a person at a terminal for a line of text, one choice
// from a list, or yes or no.
package prompt

import (
	"bufio"
	"io"
	"os"
	"strconv"
	"strings"
	"unicode"

	"github.com/pkg/errors"
	"golang.org/x/term"
)

// ErrCancelled reports that the person pressed Ctrl-C or closed the input
// instead of answering.
var ErrCancelled = errors.New("cancelled")

// Terminal asks its questions on a terminal that understands ANSI cursor
// codes. Each question holds the terminal in raw mode until it is answered,
// so Ctrl-C reaches the question as a key and cancels it.
type Terminal struct {
	// in is the terminal the answers come from.
	in *os.File
	// keys reads in one character at a time, and keeps what a question read
	// ahead for the next one.
	keys *bufio.Reader
	// out is where the questions show.
	out io.Writer
	// color is true when out shows color.
	color bool
}

// NewTerminal returns a Terminal that reads answers from in and shows its
// questions on out, in color when color is true.
func NewTerminal(in *os.File, out io.Writer, color bool) *Terminal {
	return &Terminal{in: in, keys: bufio.NewReader(in), out: out, color: color}
}

// Text asks for a line of text and returns it trimmed, or fallback when the
// person presses Enter on an empty line. While check returns an error for
// the answer, Text shows the error and asks again.
func (t *Terminal) Text(label, fallback string, check func(string) error) (string, error) {
	// Take the keys as they are pressed.
	restore, err := t.raw()
	if err != nil {
		return "", err
	}
	defer restore()

	// Edit the line one key at a time, until an answer passes the check.
	var line []rune
	for {
		t.drawText(label, string(line), fallback)
		k, r, err := t.key()
		if err != nil {
			return "", t.cancel()
		}
		switch k {
		case keyRune:
			line = append(line, r)
		case keyBackspace:
			line = line[:max(len(line)-1, 0)]
		case keyEnter:
			answer := strings.TrimSpace(string(line))
			if answer == "" {
				answer = fallback
			}
			if err := check(answer); err != nil {
				t.write("\r\n  " + t.paint("31", err.Error()) + "\r\n")
				continue
			}
			t.answered(label, answer)
			return answer, nil
		}
	}
}

// Select asks for one of options, starting on the one at selected, and
// returns the chosen option's index. The arrow keys, j and k move, a digit
// jumps to that option, and Enter chooses.
func (t *Terminal) Select(label string, options []Option, selected int) (int, error) {
	// Take the keys as they are pressed.
	restore, err := t.raw()
	if err != nil {
		return 0, err
	}
	defer restore()

	// Show the list, and redraw it in place after each key.
	t.write(t.paint("36", "?") + " " + t.paint("1", label) + t.paint("2", "  arrow keys, then Enter") + "\r\n")
	t.drawOptions(options, selected)
	for {
		k, r, err := t.key()
		if err != nil {
			t.write("\r\n")
			return 0, ErrCancelled
		}
		switch {
		case k == keyUp || k == keyRune && r == 'k':
			selected = (selected + len(options) - 1) % len(options)
		case k == keyDown || k == keyRune && r == 'j':
			selected = (selected + 1) % len(options)
		case k == keyRune && r >= '1' && int(r-'1') < len(options):
			selected = int(r - '1')
		case k == keyEnter:
			// Fold the list into one line that keeps the answer.
			t.write("\x1b[" + strconv.Itoa(len(options)+1) + "A\r\x1b[J")
			t.answered(label, options[selected].Label)
			return selected, nil
		default:
			continue
		}
		t.write("\x1b[" + strconv.Itoa(len(options)) + "A\r\x1b[J")
		t.drawOptions(options, selected)
	}
}

// Confirm asks a yes or no question and returns the answer, or fallback when
// the person presses Enter.
func (t *Terminal) Confirm(label string, fallback bool) (bool, error) {
	// Take the keys as they are pressed.
	restore, err := t.raw()
	if err != nil {
		return false, err
	}
	defer restore()

	// Show which answer Enter gives.
	keys := " (y/N) "
	if fallback {
		keys = " (Y/n) "
	}
	t.write(t.paint("36", "?") + " " + t.paint("1", label) + t.paint("2", keys))

	// Take the first key that answers.
	for {
		k, r, err := t.key()
		if err != nil {
			return false, t.cancel()
		}
		answer := fallback
		switch {
		case k == keyRune && unicode.ToLower(r) == 'y':
			answer = true
		case k == keyRune && unicode.ToLower(r) == 'n':
			answer = false
		case k != keyEnter:
			continue
		}
		word := "no"
		if answer {
			word = "yes"
		}
		t.answered(label, word)
		return answer, nil
	}
}

// raw puts the terminal in raw mode and returns the function that puts it
// back.
func (t *Terminal) raw() (func(), error) {
	fd := int(t.in.Fd())
	state, err := term.MakeRaw(fd)
	if err != nil {
		return nil, errors.Wrap(err, "read the terminal")
	}
	return func() { _ = term.Restore(fd, state) }, nil
}

// drawText redraws a Text question's line: the label, then what the person
// typed, or fallback dimmed with the cursor before it while the line is
// empty.
func (t *Terminal) drawText(label, line, fallback string) {
	text := "\r\x1b[K" + t.paint("36", "?") + " " + t.paint("1", label) + t.paint("2", " › ")
	switch {
	case line != "":
		text += line
	case fallback != "":
		text += t.paint("2", fallback) + "\x1b[" + strconv.Itoa(len([]rune(fallback))) + "D"
	}
	t.write(text)
}

// drawOptions writes one line for each option, with a pointer on the one at
// selected.
func (t *Terminal) drawOptions(options []Option, selected int) {
	var text strings.Builder
	for i, option := range options {
		line := "  " + option.Label
		if i == selected {
			line = t.paint("36", "❯ "+option.Label)
		}
		if option.Hint != "" {
			line += t.paint("2", "  "+option.Hint)
		}
		text.WriteString(line + "\r\n")
	}
	t.write(text.String())
}

// answered replaces the question on the current line with its answer.
func (t *Terminal) answered(label, answer string) {
	t.write("\r\x1b[K" + t.paint("32", "✓") + " " + label + t.paint("2", " › ") + answer + "\r\n")
}

// cancel ends the question's line and returns ErrCancelled.
func (t *Terminal) cancel() error {
	t.write("\r\n")
	return ErrCancelled
}

// paint returns text in the style of the SGR code when the terminal shows
// color, and text unchanged otherwise.
func (t *Terminal) paint(code, text string) string {
	if !t.color {
		return text
	}
	return "\x1b[" + code + "m" + text + "\x1b[0m"
}

// write shows text on the terminal.
func (t *Terminal) write(text string) {
	_, _ = io.WriteString(t.out, text)
}
