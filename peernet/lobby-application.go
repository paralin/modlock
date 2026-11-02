package peernet

import "github.com/paralin/modlock/src/host_app/control"

// lobbyApplication follows configurations offered to one local host across
// socket reconnects. A failed Apply keeps its last successful configuration.
type lobbyApplication struct {
	// offered is the most recent configuration the store sent.
	offered *control.ServiceReply
	// confirmed is the configuration the local host last adopted successfully.
	confirmed string
	// result repeats the last adoption result after a dropped socket.
	result *control.ServiceResult
}

// offer reports whether next needs to restart the mod. The store may repeat
// the successful configuration after a later Apply failed; it is already running.
func (a *lobbyApplication) offer(next *control.ServiceReply) bool {
	if next == nil || next.EqualVT(a.offered) {
		return false
	}
	a.offered = next
	return next.GetId() != a.confirmed
}

// adopt records the host's result without losing its last successful identity.
func (a *lobbyApplication) adopt(result *control.ServiceResult) {
	a.result = result
	if result.GetError() == "" {
		a.confirmed = result.GetId()
	}
}
