package ui

// Style lays out and paints an element. Colors are "#rrggbb" or "#rrggbbaa".
// A zero field is unset, except where zero means something on the wire: the
// pointer fields, and the slices and Lengths, which are unset only when nil
// or zero.
type Style struct {
	// Width sizes the element across.
	Width Length
	// Height sizes the element down.
	Height Length
	// MinWidth is the least the element's width may shrink to.
	MinWidth Length
	// MaxWidth is the most the element's width may grow to.
	MaxWidth Length
	// MinHeight is the least the element's height may shrink to.
	MinHeight Length
	// MaxHeight is the most the element's height may grow to.
	MaxHeight Length
	// Flow places the children in a column or a row, or in columns or rows
	// that wrap where one is full; unset stacks them.
	Flow Flow
	// X places the element across from its parent's left edge, in pixels or a
	// percent of the parent, outside the parent's flow.
	X Length
	// Y places the element down from its parent's top edge, as X does across.
	Y Length
	// ZIndex draws the element above its siblings with a lower index.
	ZIndex int32
	// Overflow decides what children that do not fit do, across and down.
	Overflow Overflow
	// OverflowX sets Overflow across.
	OverflowX Overflow
	// OverflowY sets Overflow down.
	OverflowY Overflow
	// HorizontalAlign places the element across: Left, Center or Right.
	HorizontalAlign Align
	// VerticalAlign places the element down: Top, Center or Bottom.
	VerticalAlign Align
	// Margin is the space outside the element's edge, as Edges takes it.
	Margin Edges
	// Padding is the space inside the element's edge, as Edges takes it.
	Padding Edges
	// Background fills the element.
	Background string
	// Color colors the text.
	Color string
	// FontSize is the text's size in pixels.
	FontSize float32
	// Bold sets the text bold.
	Bold bool
	// TextAlign aligns the text: Left, Center or Right.
	TextAlign Align
	// BorderRadius rounds the corners, in pixels.
	BorderRadius float32
	// Opacity fades the element and its children, from 0 to 1.
	Opacity *float32
	// Face is one of the game's typefaces: Body, Block or Serif.
	Face Face
	// Uppercase sets the text in capitals.
	Uppercase bool
	// LetterSpacing widens the space between letters, in pixels.
	LetterSpacing float32
	// Shadow draws a soft drop shadow under the text, to read over the world.
	Shadow bool
	// TextShadow draws this shadow under the text in place of Shadow's.
	TextShadow *Shadow
	// BorderWidth and BorderColor draw a line around the element's edge; it
	// shows only when both are set.
	BorderWidth float32
	// BorderColor is the color of the border.
	BorderColor string
	// Gradient fills the element in place of its background.
	Gradient *Gradient
	// BoxShadow draws a shadow around or inside the element's box.
	BoxShadow *Shadow
	// Tint washes the element and its children with a color.
	Tint string
	// Saturation scales the colors' saturation; 1 leaves them.
	Saturation *float32
	// Brightness scales the colors' brightness; 1 leaves them.
	Brightness *float32
	// Contrast scales the colors' contrast; 1 leaves them.
	Contrast *float32
	// Blur blurs the element and its children by a radius in pixels.
	Blur float32
	// Italic sets the text in italics.
	Italic bool
	// Weight sets the text's weight in place of Bold.
	Weight Weight
	// LineHeight is the height of each line of text, in pixels.
	LineHeight float32
	// NoWrap keeps the text on one line.
	NoWrap bool
	// TextOverflow ends text that does not fit.
	TextOverflow TextOverflow
	// Decoration draws a line under or through the text.
	Decoration Decoration
	// Translate moves the element as drawn by [x, y] pixels.
	Translate []float32
	// Scale scales the element as drawn: one factor, or [x, y].
	Scale []float32
	// Rotate turns the element clockwise, in degrees.
	Rotate float32
	// Origin is the point the element scales and turns about, as [x, y]
	// percents of its size; unset is its center.
	Origin []float32
	// Transition animates each change of the style.
	Transition *Transition
}

// Edges are distances in pixels: one for every side, [vertical, horizontal],
// or [top, right, bottom, left]. Nil is unset.
type Edges []float32

// Merge returns s with each field over sets in its place, as a later style
// overrides an earlier one.
func (s Style) Merge(over Style) Style {
	// Sizes.
	s.Width = pick(over.Width, s.Width)
	s.Height = pick(over.Height, s.Height)
	s.MinWidth = pick(over.MinWidth, s.MinWidth)
	s.MaxWidth = pick(over.MaxWidth, s.MaxWidth)
	s.MinHeight = pick(over.MinHeight, s.MinHeight)
	s.MaxHeight = pick(over.MaxHeight, s.MaxHeight)

	// Flow and placement.
	s.Flow = pick(over.Flow, s.Flow)
	s.X = pick(over.X, s.X)
	s.Y = pick(over.Y, s.Y)
	s.ZIndex = pick(over.ZIndex, s.ZIndex)
	s.Overflow = pick(over.Overflow, s.Overflow)
	s.OverflowX = pick(over.OverflowX, s.OverflowX)
	s.OverflowY = pick(over.OverflowY, s.OverflowY)

	// Alignment and spacing.
	s.HorizontalAlign = pick(over.HorizontalAlign, s.HorizontalAlign)
	s.VerticalAlign = pick(over.VerticalAlign, s.VerticalAlign)
	s.Margin = pickList(over.Margin, s.Margin)
	s.Padding = pickList(over.Padding, s.Padding)

	// Paint and text.
	s.Background = pick(over.Background, s.Background)
	s.Color = pick(over.Color, s.Color)
	s.FontSize = pick(over.FontSize, s.FontSize)
	s.Bold = s.Bold || over.Bold
	s.TextAlign = pick(over.TextAlign, s.TextAlign)

	// Box and font.
	s.BorderRadius = pick(over.BorderRadius, s.BorderRadius)
	s.Opacity = pick(over.Opacity, s.Opacity)
	s.Face = pick(over.Face, s.Face)
	s.Uppercase = s.Uppercase || over.Uppercase
	s.LetterSpacing = pick(over.LetterSpacing, s.LetterSpacing)
	s.Shadow = s.Shadow || over.Shadow

	// Shadows, borders and fills.
	s.TextShadow = pick(over.TextShadow, s.TextShadow)
	s.BorderWidth = pick(over.BorderWidth, s.BorderWidth)
	s.BorderColor = pick(over.BorderColor, s.BorderColor)
	s.Gradient = pick(over.Gradient, s.Gradient)
	s.BoxShadow = pick(over.BoxShadow, s.BoxShadow)
	s.Tint = pick(over.Tint, s.Tint)

	// Filters.
	s.Saturation = pick(over.Saturation, s.Saturation)
	s.Brightness = pick(over.Brightness, s.Brightness)
	s.Contrast = pick(over.Contrast, s.Contrast)
	s.Blur = pick(over.Blur, s.Blur)

	// Text shape.
	s.Italic = s.Italic || over.Italic
	s.Weight = pick(over.Weight, s.Weight)
	s.LineHeight = pick(over.LineHeight, s.LineHeight)
	s.NoWrap = s.NoWrap || over.NoWrap
	s.TextOverflow = pick(over.TextOverflow, s.TextOverflow)
	s.Decoration = pick(over.Decoration, s.Decoration)

	// Transform and motion.
	s.Translate = pickList(over.Translate, s.Translate)
	s.Scale = pickList(over.Scale, s.Scale)
	s.Rotate = pick(over.Rotate, s.Rotate)
	s.Origin = pickList(over.Origin, s.Origin)
	s.Transition = pick(over.Transition, s.Transition)
	return s
}

// pick returns over when it is set, and else base.
func pick[T comparable](over, base T) T {
	var zero T
	if over != zero {
		return over
	}
	return base
}

// pickList returns over when it is set, and else base.
func pickList[T any](over, base []T) []T {
	if over != nil {
		return over
	}
	return base
}
