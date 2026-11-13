package theme

import "github.com/paralin/modlock/mod/ui"

// Type is one type role: its face, size in pixels, and whether in capitals.
type Type struct {
	// Face is the role's typeface.
	Face ui.Face
	// Size is the role's size in pixels.
	Size float32
	// Uppercase sets the role in capitals.
	Uppercase bool
}
