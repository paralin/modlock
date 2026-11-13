package ui

import "github.com/paralin/modlock/mod"

// AbilityTip is the game's own window for an ability, in the player's
// language: Name is the ability's class name, such as "ability_bebop_hook",
// and Player is whose hero the window reads its numbers from, a hero that
// stands on the map, such as a bot that plays it.
type AbilityTip struct {
	// Name is the ability's class name.
	Name string
	// Player is whose hero the window reads its numbers from.
	Player mod.Player
}
