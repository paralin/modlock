package mod

import "time"

// Seating is where SeatPlayers puts each player who joins.
type Seating struct {
	// Hero is each player's hero: its name as players know it or as the game
	// does, such as "haze" or "hero_haze", its id as an int or uint32, or
	// AnyHero for the hero they have or else a random one. Nil is AnyHero.
	Hero any
	// Team is the team, 2 or 3, every player joins. Zero puts each on the
	// side with fewer heroes. TeamOf replaces it when set.
	Team int32
	// TeamOf picks the team, 2 or 3, for one player as they join, or nil.
	TeamOf func(connection *Connection) int32
	// Respawn is how long a fallen hero waits before it comes back. Zero
	// leaves a fallen hero to the game.
	Respawn time.Duration
}

// SeatPlayers gives every player who joins a hero on a team, skipping the
// game's team and hero screens, and keeps them seated for the rest of the
// game: it brings a fallen hero back after Respawn and seats a player again
// after a hero change. It returns the Cast that does the seating, whose
// members are the players' connections, and runs it each frame itself.
func SeatPlayers(seating Seating) *Cast[*Connection] {
	// Without a hero, any hero does.
	hero := seating.Hero
	if hero == nil {
		hero = AnyHero
	}

	// Decide each player's seat once, as they join, and keep it by slot.
	wants := map[int32]Want{}
	cast := NewCast(CastOptions[*Connection]{
		Join: func(connection *Connection) *Connection {
			team := seating.Team
			if seating.TeamOf != nil {
				team = seating.TeamOf(connection)
			}
			wants[connection.GetPlayer()] = Want{Hero: hero, Team: team, Respawn: seating.Respawn}
			return connection
		},
		Leave: func(_ *Connection, connection *Connection) {
			delete(wants, connection.GetPlayer())
		},
		WantOf: func(member *Connection) Want {
			return wants[member.GetPlayer()]
		},
	})

	// Run the cast with the mod's frames.
	OnFrame(func(frame *FrameEvent) {
		cast.Frame(frame.GetTimeSeconds())
	})
	return cast
}
