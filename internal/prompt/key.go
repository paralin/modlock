package prompt

import "unicode"

// key is the kind of one key press.
type key int

// The keys a question acts on.
const (
	// keyOther is a key no question uses.
	keyOther key = iota
	// keyRune is a printable character.
	keyRune
	// keyEnter is Enter.
	keyEnter
	// keyBackspace is Backspace.
	keyBackspace
	// keyUp is the up arrow.
	keyUp
	// keyDown is the down arrow.
	keyDown
)

// key reads one key press from a terminal in raw mode, with its character
// for a keyRune. It returns ErrCancelled for Ctrl-C, Ctrl-D and the end of
// the input.
func (t *Terminal) key() (key, rune, error) {
	// Read the key's first character.
	r, _, err := t.keys.ReadRune()
	if err != nil {
		return keyOther, 0, ErrCancelled
	}

	// Sort it into the keys the questions use.
	switch r {
	case 0x03, 0x04:
		return keyOther, 0, ErrCancelled
	case '\r', '\n':
		return keyEnter, 0, nil
	case 0x7f, '\b':
		return keyBackspace, 0, nil
	case 0x1b:
		return t.escape(), 0, nil
	}
	if unicode.IsPrint(r) {
		return keyRune, r, nil
	}
	return keyOther, 0, nil
}

// escape reads the rest of an escape sequence, whose first character key
// took, and returns the arrow key it stands for or keyOther. An arrow
// arrives as ESC [ A or, from a keypad in application mode, ESC O A.
func (t *Terminal) escape() key {
	// A lone Escape has nothing after it; leave the next key unread.
	if t.keys.Buffered() == 0 {
		return keyOther
	}
	if r, _, err := t.keys.ReadRune(); err != nil || r != '[' && r != 'O' {
		return keyOther
	}

	// Skip the sequence's parameters to the letter that ends it.
	for {
		r, _, err := t.keys.ReadRune()
		switch {
		case err != nil:
			return keyOther
		case r == 'A':
			return keyUp
		case r == 'B':
			return keyDown
		case r >= 0x40 && r <= 0x7e:
			return keyOther
		}
	}
}
