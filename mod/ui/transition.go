package ui

// Transition animates each change of an element's style, and the move to its
// hover style and back, over Seconds after Delay. Easing is EaseOut unless
// set.
type Transition struct {
	// Seconds is how long each change takes.
	Seconds float32
	// Delay is how long each change waits before it starts, in seconds.
	Delay float32
	// Easing is EaseOut, EaseIn, EaseInOut or Linear.
	Easing Easing
}
