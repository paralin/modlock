package ui

// Timeline is a timed presentation. Its steps run on the player's own time,
// or, with Game set, hold while the game is paused.
type Timeline struct {
	// Game makes the timeline follow the game clock.
	Game bool
	// Steps lists the timed presentation changes.
	Steps []TimelineStep
}
