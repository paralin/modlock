package mod

import (
	"slices"

	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/players"
)

// roster returns the player list the host last reported, or asks for it.
func roster() []*Connection {
	if held, _ := connections.snapshot(); held != nil {
		return held
	}
	held, _ := Players()
	return held
}

// Player addresses one connected player by their server slot. Two Players
// are equal only when they name the same connection: a person who takes a
// slot another left gets a different Player, so a map keyed by Player never
// hands the newcomer the entry of the one who left. PlayerOf makes one; the
// zero Player names nobody.
type Player struct {
	// slot identifies the player on the server.
	slot int32
	// tenure is one more than the number of connections the slot had lost
	// when the Player was made, so it is zero only in the zero Player.
	tenure uint32
}

// PlayerOf returns the Player of the connection that holds slot now, equal
// to every other Player made for that connection.
func PlayerOf(slot int32) Player {
	return connections.player(slot)
}

// Slot returns the server slot the player holds.
func (p Player) Slot() int32 {
	return p.slot
}

// Connection returns the player's connection: their name, Steam ID, whether
// they are a bot and whether they finished joining. It returns nil when
// nobody holds the slot.
func (p Player) Connection() *Connection {
	for _, connection := range roster() {
		if connection.GetPlayer() == p.slot {
			return connection
		}
	}
	return nil
}

// ConnectionOfSteamID returns the connection of the account with a Steam ID,
// or nil when it is not connected. Its GetPlayer is the slot of the Player to
// call.
func ConnectionOfSteamID(steamID uint64) *Connection {
	for _, connection := range roster() {
		if connection.GetSteamId() == steamID {
			return connection
		}
	}
	return nil
}

// Players lists the occupied player slots in slot order. It answers from the
// roster the host last reported, and asks the host until a report arrives.
func Players() ([]*Connection, error) {
	if held, _ := connections.snapshot(); held != nil {
		return slices.Clone(held), nil
	}
	response := &players.PlayersResponse{}
	if err := invoke("Players", &wasm.Empty{}, response); err != nil {
		return nil, err
	}
	return response.GetPlayers(), nil
}
