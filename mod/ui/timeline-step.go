package ui

// TimelineStep is one moment of a timeline, At seconds from its start, 0 to
// 60. From then on the elements whose Measure the step names take its look:
// Class adds classes from the mod's sheets, separated by spaces, and Style is
// laid over each element's own, setting, as a hover does, only what the
// element's style sets or what has a resting value. A step's look replaces
// the look an earlier step gave the same measure. Sound plays the game's
// sound event, cut after Seconds unless Seconds is zero.
type TimelineStep struct {
	// At is the offset in seconds from the start.
	At float32
	// Measure selects the elements this step changes.
	Measure string
	// Class adds classes from the mod style sheets.
	Class string
	// Style overlays the selected elements.
	Style *Style
	// Sound plays the named game sound event.
	Sound string
	// Seconds bounds the sound duration when nonzero.
	Seconds float32
}
