package hud

// ActionKind is how much an action draws the eye.
type ActionKind string

// The action kinds.
const (
	// Secondary is every action but the one a screen leads with; it is the
	// default.
	Secondary ActionKind = ""
	// Primary is the one action a screen leads with.
	Primary ActionKind = "primary"
	// Quiet is an action that should not draw the eye.
	Quiet ActionKind = "quiet"
)
