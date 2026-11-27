package ui

// RangeHandle is one handle of a range: its name, 1 to 64 letters, digits,
// '_', '-' or '.', distinct within the range, and its value.
type RangeHandle struct {
	// Name names the handle in the range's reports.
	Name string
	// Value is where the handle sits on the track.
	Value float32
}
