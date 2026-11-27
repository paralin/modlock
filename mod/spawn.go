package mod

// respawnedEvent is the game's modifier event for a hero entering play.
const respawnedEvent = "PLAYER_RESPAWNED"

// spawnTracker filters repeated reports of a hero entering play.
type spawnTracker struct {
	// frame counts frames, so heardAt can tell one frame's events apart.
	frame uint64
	// heardAt maps each slot to the frame the handlers last heard of its
	// hero; the game repeats the event as a player's hero is built.
	heardAt map[int32]uint64
}

// spawns tracks the default module's spawn deduplication window.
var spawns spawnTracker

// OnSpawn calls handler each time a player's hero enters play: their first
// hero after joining, and each respawn. The
// player has a hero to move, equip and read from then on, which OnPlayerJoin
// does not promise. The library hears it from the game's PLAYER_RESPAWNED
// event, before the frame handlers run.
func OnSpawn(handler func(player Player, pawn *Pawn)) {
	defaultRegistry.OnSpawn(handler)
}

// nextFrame advances the deduplication window before frame handlers.
func (s *spawnTracker) nextFrame() { s.frame++ }

// respawned tells the spawn handlers that the player's hero entered play.
func (s *spawnTracker) respawned(player Player, notify func(Player, *Pawn)) {
	// Skip a placeholder pawn, which has hero 0, and a hero already told of this frame.
	if player.Slot() < 0 {
		return
	}
	pawn, err := player.Pawn()
	if err != nil || pawn == nil || pawn.GetHero() == 0 || pawn.GetHealth() <= 0 {
		return
	}
	if frame, ok := s.heardAt[player.Slot()]; ok && frame == s.frame {
		return
	}
	if s.heardAt == nil {
		s.heardAt = map[int32]uint64{}
	}
	s.heardAt[player.Slot()] = s.frame

	// Deliver the accepted spawn before named modifier callbacks.
	notify(player, pawn)
}
