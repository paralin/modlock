package mod

// connectionSet keeps the reported roster and distinguishes replacements in a slot.
type connectionSet struct {
	// roster is nil until the first host report, including an empty report.
	roster []*Connection
	// rosters counts reports so Cast settles each new list once.
	rosters uint64
	// generations records the last generation in each occupied slot.
	generations map[int32]uint32
	// tenures counts the connections each slot has lost since module start.
	tenures map[int32]uint32
}

// connections tracks the default module's connections.
var connections = connectionSet{tenures: map[int32]uint32{}}

// update publishes the list and ends each departed connection's tenure before callbacks.
func (c *connectionSet) update(event *PlayersChangedEvent) {
	// Keep the list.
	c.roster = event.GetPlayers()
	if c.roster == nil {
		c.roster = []*Connection{}
	}
	c.rosters++

	// End each tenure whose connection is gone, then note the new list's.
	current := make(map[int32]uint32, len(c.roster))
	for _, connection := range c.roster {
		current[connection.GetPlayer()] = connection.GetGeneration()
	}
	for slot, generation := range c.generations {
		if held, ok := current[slot]; !ok || held != generation {
			c.tenures[slot]++
		}
	}
	c.generations = current
}

// snapshot borrows the current list and revision until the next host entry.
func (c *connectionSet) snapshot() ([]*Connection, uint64) {
	return c.roster, c.rosters
}

// player returns the identity of the current holder of slot.
func (c *connectionSet) player(slot int32) Player {
	return Player{slot: slot, tenure: c.tenures[slot] + 1}
}

// holds reports whether p names the current connection tenure.
func (c *connectionSet) holds(p Player) bool {
	return p.tenure != 0 && p.tenure == c.tenures[p.slot]+1
}
