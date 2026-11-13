package ui

import "github.com/paralin/modlock/mod"

// AbilityRow is the four abilities, in slot order, AbilityDisplay shows for
// one player.
type AbilityRow struct {
	// Player is whose abilities the row replaces.
	Player mod.Player
	// Abilities holds slots 0 to 3; a nil slot shows the game's own.
	Abilities []*ShownAbility
}
