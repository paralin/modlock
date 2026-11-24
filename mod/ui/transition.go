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
	// Curve shapes the change in place of Easing as a cubic Bezier curve's
	// two control points, [x1, y1, x2, y2], as CSS's cubic-bezier: each x is a
	// share of the time from 0 to 1, and a y outside 0 to 1 overshoots.
	Curve []float32
}

// Clipping draws only part of an element and its children. Rect is a box,
// [top, right, bottom, left], each side a percent of the element's size from
// its top or left edge. Without Rect, the clip is a slice of a circle about
// Center, [x, y] percents of the element's size and its middle unless set,
// through Sweep degrees from Start, as the game draws a cooldown ring. A
// transition does not ease a clip; draw it again to fill a ring over time.
type Clipping struct {
	Rect   []float32
	Center []float32
	Start  float32
	Sweep  float32
}
