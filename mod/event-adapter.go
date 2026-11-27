package mod

// eventAdapter composes event delivery with the default module's runtime state.
type eventAdapter struct {
	// Registry supplies ordinary event delivery.
	*Registry
	// connections publishes identities before roster callbacks run.
	connections *connectionSet
	// calls completes outbound service requests.
	calls *serviceCalls
}

// registered is the receiver of the generated serveMod adapter.
var registered = &eventAdapter{Registry: &defaultRegistry, connections: &connections, calls: &calls}

// Frame advances timers and spawn bookkeeping before registered frame callbacks.
func (a *eventAdapter) Frame(event *FrameEvent) error {
	advanceTimers(event.GetTimeSeconds())
	spawns.nextFrame()
	return a.Registry.Frame(event)
}

// ModifierFired reports a spawn before the named modifier callbacks.
func (a *eventAdapter) ModifierFired(event *ModifierFiredEvent) error {
	if event.GetName() == respawnedEvent && a.Registry.watchesSpawns() {
		spawns.respawned(PlayerOf(event.GetPlayer()), a.Registry.Spawn)
	}
	return a.Registry.ModifierFired(event)
}

// PlayersChanged publishes connection tenures before notifying roster watchers.
func (a *eventAdapter) PlayersChanged(event *PlayersChangedEvent) error {
	a.connections.update(event)
	roster, _ := a.connections.snapshot()
	return a.Registry.PlayersChanged(roster)
}

// ServiceReplied removes a pending call before invoking its completion.
func (a *eventAdapter) ServiceReplied(event *ServiceRepliedEvent) error {
	return a.calls.ServiceReplied(event)
}

var _ modHandlers = (*eventAdapter)(nil)
