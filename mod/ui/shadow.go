package ui

// Shadow is a soft shadow, offset by X and Y and softened by Blur, in pixels.
// Spread grows a box's shadow in pixels, or strengthens a text shadow, where 0
// draws it at full strength; Inset draws a box's shadow inside its edge.
type Shadow struct {
	// X offsets the shadow across, in pixels.
	X float32
	// Y offsets the shadow down, in pixels.
	Y float32
	// Blur softens the shadow, in pixels.
	Blur float32
	// Spread grows a box's shadow or strengthens a text shadow.
	Spread float32
	// Color is the shadow's color, as "#rrggbb" or "#rrggbbaa".
	Color string
	// Inset draws a box's shadow inside its edge.
	Inset bool
}
