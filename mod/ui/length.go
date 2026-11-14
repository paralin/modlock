package ui

// Length is a size: pixels, a percentage of the parent, FitChildren to size to
// the children, or Fill to share the space left in the parent's flow. The zero
// Length is unset.
type Length struct {
	// kind is which form the length takes.
	kind lengthKind
	// value is the pixels or the percent.
	value float32
}

// lengthKind is the form a Length takes.
type lengthKind uint8

const (
	// unsetLength is the zero Length, which leaves the size to the renderer.
	unsetLength lengthKind = iota
	// pixelLength is a size in pixels.
	pixelLength
	// percentLength is a percentage of the parent.
	percentLength
	// fitLength sizes the element to its children.
	fitLength
	// fillLength shares the space left in the parent's flow.
	fillLength
)

// FitChildren sizes an element to its children.
var FitChildren = Length{kind: fitLength}

// Fill shares the space left in the parent's flow.
var Fill = Length{kind: fillLength}

// Px returns a length of v pixels.
func Px(v float32) Length {
	return Length{kind: pixelLength, value: v}
}

// Percent returns a length of v percent of the parent.
func Percent(v float32) Length {
	return Length{kind: percentLength, value: v}
}
