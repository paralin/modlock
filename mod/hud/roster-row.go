package hud

import "github.com/paralin/modlock/mod"

// RosterRow is one player as the roster shows them.
type RosterRow struct {
	// Name is the player's name.
	Name string
	// Self marks the viewing player's own row.
	Self bool
	// Hero is the player's hero as the pawn reports it, or 0 without one.
	Hero uint32
	// Activity is the line under the name, such as "Free roam".
	Activity string
	// SteamID badges the portrait's corner with the player's Steam avatar;
	// 0 shows none.
	SteamID uint64
	// OnPress makes the row a button the viewer presses while they have a
	// cursor, such as to open a card about that player.
	OnPress func(p mod.Player)
	// Selected edges the portrait in the accent, as for the row a card shows.
	Selected bool
}
