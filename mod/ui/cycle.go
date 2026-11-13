package ui

// Cycle swings an element from its style to Style and back, each way taking
// Seconds, Count times or, with a zero Count, while it shows.
type Cycle struct {
	// Style is the style the element swings to.
	Style Style
	// Seconds is how long each swing takes.
	Seconds float32
	// Count is how many times it swings, or 0 for as long as it shows.
	Count uint32
}
