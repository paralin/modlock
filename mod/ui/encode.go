package ui

import uipb "github.com/paralin/modlock/src/wasm/ui"

// emptyStyle is the encoding of a style with nothing set.
var emptyStyle = &uipb.Style{}

// wireStyle encodes style, or returns nil when it sets nothing.
func wireStyle(style *Style) *uipb.Style {
	// Encode every field, then drop a style that sets none.
	if style == nil {
		return nil
	}
	overflowX, overflowY := pick(style.OverflowX, style.Overflow), pick(style.OverflowY, style.Overflow)
	w := &uipb.Style{
		Width:           length(style.Width),
		Height:          length(style.Height),
		Flow:            flow(style.Flow),
		HorizontalAlign: align(style.HorizontalAlign),
		VerticalAlign:   align(style.VerticalAlign),
		Margin:          edges(style.Margin),
		Padding:         edges(style.Padding),
		Background:      color(style.Background),
		Color:           color(style.Color),
		FontSize:        style.FontSize,
		Bold:            style.Bold,
		TextAlign:       align(style.TextAlign),
		BorderRadius:    style.BorderRadius,
		Opacity:         copied(style.Opacity),
		Face:            face(style.Face),
		Uppercase:       style.Uppercase,
		LetterSpacing:   style.LetterSpacing,
		Shadow:          style.Shadow,
		BorderWidth:     style.BorderWidth,
		BorderColor:     color(style.BorderColor),
		X:               length(style.X),
		Y:               length(style.Y),
		MinWidth:        length(style.MinWidth),
		MaxWidth:        length(style.MaxWidth),
		MinHeight:       length(style.MinHeight),
		MaxHeight:       length(style.MaxHeight),
		ZIndex:          style.ZIndex,
		OverflowX:       overflow(overflowX),
		OverflowY:       overflow(overflowY),
		Gradient:        gradient(style.Gradient),
		BoxShadow:       shadow(style.BoxShadow),
		TextShadow:      shadow(style.TextShadow),
		Tint:            color(style.Tint),
		Saturation:      copied(style.Saturation),
		Brightness:      copied(style.Brightness),
		Contrast:        copied(style.Contrast),
		Blur:            style.Blur,
		Italic:          style.Italic,
		Weight:          weight(style.Weight),
		LineHeight:      style.LineHeight,
		NoWrap:          style.NoWrap,
		TextOverflow:    textOverflow(style.TextOverflow),
		Decoration:      decoration(style.Decoration),
		Transform:       transform(style),
		Transition:      transition(style.Transition),
	}
	if w.EqualVT(emptyStyle) {
		return nil
	}
	return w
}

// copied returns a copy of an optional value, so the screen kept for the next
// diff holds none of the mod's pointers.
func copied(value *float32) *float32 {
	if value == nil {
		return nil
	}
	return new(*value)
}

// length encodes a length, or nil when it is unset.
func length(value Length) *uipb.Length {
	switch value.kind {
	case pixelLength:
		return &uipb.Length{Value: &uipb.Length_Pixels{Pixels: value.value}}
	case percentLength:
		return &uipb.Length{Value: &uipb.Length_Percent{Percent: value.value}}
	case fitLength:
		return &uipb.Length{Value: &uipb.Length_FitChildren{FitChildren: true}}
	case fillLength:
		return &uipb.Length{Value: &uipb.Length_Fill{Fill: 1}}
	}
	return nil
}

// edges encodes edges: one for every side, [vertical, horizontal], or [top,
// right, bottom, left].
func edges(value Edges) *uipb.Edges {
	switch len(value) {
	case 0:
		return nil
	case 1:
		return &uipb.Edges{Top: value[0], Right: value[0], Bottom: value[0], Left: value[0]}
	case 2:
		return &uipb.Edges{Top: value[0], Right: value[1], Bottom: value[0], Left: value[1]}
	case 3:
		return &uipb.Edges{Top: value[0], Right: value[1], Bottom: value[2]}
	}
	return &uipb.Edges{Top: value[0], Right: value[1], Bottom: value[2], Left: value[3]}
}

// color encodes "#rrggbb" or "#rrggbbaa" as 0xRRGGBBAA, or 0 when it is not a
// color.
func color(value string) uint32 {
	// Read the hex digits after an optional "#".
	if len(value) != 0 && value[0] == '#' {
		value = value[1:]
	}
	if len(value) != 6 && len(value) != 8 {
		return 0
	}
	var rgba uint32
	for i := 0; i < len(value); i++ {
		digit, ok := hexDigit(value[i])
		if !ok {
			return 0
		}
		rgba = rgba<<4 | digit
	}

	// A color without alpha is opaque.
	if len(value) == 6 {
		return rgba<<8 | 0xff
	}
	return rgba
}

// hexDigit returns the value of one hex digit.
func hexDigit(c byte) (uint32, bool) {
	switch {
	case c >= '0' && c <= '9':
		return uint32(c - '0'), true
	case c >= 'a' && c <= 'f':
		return uint32(c-'a') + 10, true
	case c >= 'A' && c <= 'F':
		return uint32(c-'A') + 10, true
	}
	return 0, false
}

// gradient encodes a gradient, spacing colors without places evenly.
func gradient(value *Gradient) *uipb.Gradient {
	// No gradient encodes as none.
	if value == nil {
		return nil
	}

	// Each color sits at its place, or evenly along the gradient.
	last := float32(max(1, len(value.Colors)-1))
	stops := make([]*uipb.GradientStop, len(value.Colors))
	for i, each := range value.Colors {
		at := float32(i) / last
		if i < len(value.At) {
			at = value.At[i]
		}
		stops[i] = &uipb.GradientStop{At: at, Color: color(each)}
	}
	return &uipb.Gradient{Shape: shape(value.Shape), Stops: stops}
}

// shadow encodes a shadow.
func shadow(value *Shadow) *uipb.Shadow {
	if value == nil {
		return nil
	}
	return &uipb.Shadow{
		X:      value.X,
		Y:      value.Y,
		Blur:   value.Blur,
		Spread: value.Spread,
		Color:  color(value.Color),
		Inset:  value.Inset,
	}
}

// transform encodes a style's move, scale, turn and origin, or nil for none.
func transform(style *Style) *uipb.Transform {
	// A style that moves, scales, turns or sets an origin has a transform.
	if style.Translate == nil && style.Scale == nil && style.Rotate == 0 && style.Origin == nil {
		return nil
	}
	t := &uipb.Transform{Rotate: style.Rotate}
	if len(style.Translate) != 0 {
		t.TranslateX = style.Translate[0]
	}
	if len(style.Translate) > 1 {
		t.TranslateY = style.Translate[1]
	}

	// One scale factor scales both ways. The encoding copies each value, so
	// the screen kept for the next diff holds none of the mod's slices.
	switch len(style.Scale) {
	case 1:
		t.ScaleX, t.ScaleY = new(style.Scale[0]), new(style.Scale[0])
	case 2:
		t.ScaleX, t.ScaleY = new(style.Scale[0]), new(style.Scale[1])
	}
	if len(style.Origin) != 0 {
		t.OriginX = new(style.Origin[0])
	}
	if len(style.Origin) > 1 {
		t.OriginY = new(style.Origin[1])
	}
	return t
}

// transition encodes a transition.
func transition(value *Transition) *uipb.Transition {
	if value == nil {
		return nil
	}
	return &uipb.Transition{Seconds: value.Seconds, Delay: value.Delay, Easing: easing(value.Easing)}
}

// flow encodes a flow.
func flow(value Flow) uipb.Flow {
	switch value {
	case Down:
		return uipb.Flow_FLOW_DOWN
	case Right:
		return uipb.Flow_FLOW_RIGHT
	case DownWrap:
		return uipb.Flow_FLOW_DOWN_WRAP
	case RightWrap:
		return uipb.Flow_FLOW_RIGHT_WRAP
	}
	return uipb.Flow_FLOW_NONE
}

// align encodes an alignment: left and top start, right and bottom end.
func align(value Align) uipb.Align {
	switch value {
	case Center:
		return uipb.Align_ALIGN_CENTER
	case Right, Bottom:
		return uipb.Align_ALIGN_END
	}
	return uipb.Align_ALIGN_START
}

// overflow encodes an overflow.
func overflow(value Overflow) uipb.Overflow {
	switch value {
	case Squish:
		return uipb.Overflow_OVERFLOW_SQUISH
	case Clip:
		return uipb.Overflow_OVERFLOW_CLIP
	case Scroll:
		return uipb.Overflow_OVERFLOW_SCROLL
	case Visible:
		return uipb.Overflow_OVERFLOW_SHOW
	}
	return uipb.Overflow_OVERFLOW_DEFAULT
}

// face encodes a typeface.
func face(value Face) uipb.Face {
	switch value {
	case Body:
		return uipb.Face_FACE_BODY
	case Block:
		return uipb.Face_FACE_BLOCK
	case Serif:
		return uipb.Face_FACE_SERIF
	}
	return uipb.Face_FACE_DEFAULT
}

// weight encodes a text weight.
func weight(value Weight) uipb.Weight {
	switch value {
	case Thin:
		return uipb.Weight_WEIGHT_THIN
	case Light:
		return uipb.Weight_WEIGHT_LIGHT
	case Normal:
		return uipb.Weight_WEIGHT_NORMAL
	case Medium:
		return uipb.Weight_WEIGHT_MEDIUM
	case SemiBold:
		return uipb.Weight_WEIGHT_SEMI_BOLD
	case Bold:
		return uipb.Weight_WEIGHT_BOLD
	case Black:
		return uipb.Weight_WEIGHT_BLACK
	}
	return uipb.Weight_WEIGHT_DEFAULT
}

// textOverflow encodes a text overflow.
func textOverflow(value TextOverflow) uipb.TextOverflow {
	switch value {
	case Ellipsis:
		return uipb.TextOverflow_TEXT_OVERFLOW_ELLIPSIS
	case Shrink:
		return uipb.TextOverflow_TEXT_OVERFLOW_SHRINK
	case Clip:
		return uipb.TextOverflow_TEXT_OVERFLOW_CLIP
	}
	return uipb.TextOverflow_TEXT_OVERFLOW_DEFAULT
}

// decoration encodes a text decoration.
func decoration(value Decoration) uipb.Decoration {
	switch value {
	case Underline:
		return uipb.Decoration_DECORATION_UNDERLINE
	case Strike:
		return uipb.Decoration_DECORATION_STRIKE
	}
	return uipb.Decoration_DECORATION_NONE
}

// shape encodes a gradient's shape.
func shape(value Shape) uipb.GradientShape {
	switch value {
	case Right:
		return uipb.GradientShape_GRADIENT_SHAPE_RIGHT
	case Up:
		return uipb.GradientShape_GRADIENT_SHAPE_UP
	case Left:
		return uipb.GradientShape_GRADIENT_SHAPE_LEFT
	case Radial:
		return uipb.GradientShape_GRADIENT_SHAPE_RADIAL
	}
	return uipb.GradientShape_GRADIENT_SHAPE_DOWN
}

// easing encodes an easing.
func easing(value Easing) uipb.Easing {
	switch value {
	case EaseIn:
		return uipb.Easing_EASING_EASE_IN
	case EaseInOut:
		return uipb.Easing_EASING_EASE_IN_OUT
	case Linear:
		return uipb.Easing_EASING_LINEAR
	}
	return uipb.Easing_EASING_EASE_OUT
}

// scaling encodes a way to fit a picture.
func scaling(value Scaling) uipb.Scaling {
	switch value {
	case Fit:
		return uipb.Scaling_SCALING_FIT
	case Cover:
		return uipb.Scaling_SCALING_COVER
	case Stretch:
		return uipb.Scaling_SCALING_STRETCH
	case FitWidth:
		return uipb.Scaling_SCALING_FIT_WIDTH
	case FitHeight:
		return uipb.Scaling_SCALING_FIT_HEIGHT
	case None:
		return uipb.Scaling_SCALING_NONE
	}
	return uipb.Scaling_SCALING_DEFAULT
}

// heroLook encodes a picture of a hero.
func heroLook(value HeroLook) uipb.HeroLook {
	switch value {
	case Card:
		return uipb.HeroLook_HERO_LOOK_CARD
	case Icon:
		return uipb.HeroLook_HERO_LOOK_ICON
	case Vertical:
		return uipb.HeroLook_HERO_LOOK_VERTICAL
	case TopBar:
		return uipb.HeroLook_HERO_LOOK_TOP_BAR
	case Minimap:
		return uipb.HeroLook_HERO_LOOK_MINIMAP
	}
	return uipb.HeroLook_HERO_LOOK_SMALL
}

// hudPart encodes a part of the game's HUD.
func hudPart(value HudPart) uipb.HudPart {
	switch value {
	case LevelUp:
		return uipb.HudPart_HUD_PART_LEVEL_UP
	case OwnHealth:
		return uipb.HudPart_HUD_PART_OWN_HEALTH
	case TopBar:
		return uipb.HudPart_HUD_PART_TOP_BAR
	case Ammo:
		return uipb.HudPart_HUD_PART_AMMO
	case Souls:
		return uipb.HudPart_HUD_PART_SOULS
	case Minimap:
		return uipb.HudPart_HUD_PART_MINIMAP
	case Abilities:
		return uipb.HudPart_HUD_PART_ABILITIES
	case Status:
		return uipb.HudPart_HUD_PART_STATUS
	case Stats:
		return uipb.HudPart_HUD_PART_STATS
	case HeroPicker:
		return uipb.HudPart_HUD_PART_HERO_PICKER
	case BrokerCountdown:
		return uipb.HudPart_HUD_PART_BROKER_COUNTDOWN
	case All:
		return uipb.HudPart_HUD_PART_ALL
	}
	return uipb.HudPart_HUD_PART_UNKNOWN
}
