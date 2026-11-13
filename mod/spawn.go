package mod

// spawns holds the spawn handlers and the heroes they last heard of.
var spawns struct {
	// handlers run in registration order.
	handlers []func(Player, *Pawn)
	// entities maps each slot whose hero is in play to that hero's entity.
	entities map[int32]uint32
}

// OnSpawn calls handler each time a player's hero enters play: their first
// hero after joining, each respawn, and each new hero after a change. The
// player has a hero to move, equip and read from then on, which OnPlayerJoin
// does not promise. The library reads each player's pawn once a frame while
// a handler is registered.
func OnSpawn(handler func(player Player, pawn *Pawn)) {
	spawns.handlers = append(spawns.handlers, handler)
}

// watchSpawns tells the spawn handlers of each hero that entered play since
// the last frame.
func watchSpawns() {
	// Without the player list this frame, keep what the handlers heard.
	connections, err := Players()
	if err != nil {
		return
	}

	// A hero is in play while it lives; a placeholder pawn has hero 0.
	present := make(map[int32]uint32, len(connections))
	for _, connection := range connections {
		player := Player{Slot: connection.GetPlayer()}
		pawn, err := player.Pawn()
		if err != nil || pawn == nil || pawn.GetHero() == 0 || pawn.GetHealth() <= 0 {
			continue
		}
		present[player.Slot] = pawn.GetEntity()

		// A respawn follows a death, and a new hero has a new entity.
		if entity, ok := spawns.entities[player.Slot]; ok && entity == pawn.GetEntity() {
			continue
		}
		for _, handler := range spawns.handlers {
			handler(player, pawn)
		}
	}

	// Forget the fallen and the gone, so their next hero is a spawn.
	spawns.entities = present
}
