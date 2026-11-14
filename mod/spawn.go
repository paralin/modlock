package mod

// respawnedEvent is the game's modifier event for a hero entering play.
const respawnedEvent = "PLAYER_RESPAWNED"

// spawns holds the spawn handlers and when they last heard of each hero.
var spawns struct {
	// handlers run in registration order.
	handlers []func(Player, *Pawn)
	// frame counts frames, so heardAt can tell one frame's events apart.
	frame uint64
	// heardAt maps each slot to the frame the handlers last heard of its
	// hero; the game repeats the event as a player's hero is built.
	heardAt map[int32]uint64
}

// OnSpawn calls handler each time a player's hero enters play: their first
// hero after joining, and each respawn. The
// player has a hero to move, equip and read from then on, which OnPlayerJoin
// does not promise. The library hears it from the game's PLAYER_RESPAWNED
// event, before the frame handlers run.
func OnSpawn(handler func(player Player, pawn *Pawn)) {
	spawns.handlers = append(spawns.handlers, handler)
}

// respawned tells the spawn handlers that the player's hero entered play.
func respawned(player Player) {
	// Skip a placeholder pawn, which has hero 0, and a hero already told of this frame.
	if player.Slot() < 0 {
		return
	}
	pawn, err := player.Pawn()
	if err != nil || pawn == nil || pawn.GetHero() == 0 || pawn.GetHealth() <= 0 {
		return
	}
	if frame, ok := spawns.heardAt[player.Slot()]; ok && frame == spawns.frame {
		return
	}
	if spawns.heardAt == nil {
		spawns.heardAt = map[int32]uint64{}
	}
	spawns.heardAt[player.Slot()] = spawns.frame

	// Tell each handler.
	for _, handler := range spawns.handlers {
		handler(player, pawn)
	}
}
