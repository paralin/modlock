package ui

// Gradient blends colors across an element in place of its background. The
// colors run down unless Shape is set; At places each color from 0 to 1,
// spaced evenly where unset.
type Gradient struct {
	// Shape is the direction the colors run: Down, Right, Up, Left or Radial.
	Shape Shape
	// Colors are the colors in order, as "#rrggbb" or "#rrggbbaa".
	Colors []string
	// At places each color from 0 to 1; a color past its end is spaced evenly.
	At []float32
}
