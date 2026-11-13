package prompt

// Option is one choice a Select offers.
type Option struct {
	// Label is the choice as the person reads it.
	Label string
	// Hint is a short note shown dimmed after the label, or empty.
	Hint string
}
