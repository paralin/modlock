package mod

import "time"

// Seating is the hero and team Seat gives one player.
type Seating struct {
	// Hero is the hero's name as players know it or as the game does, such as
	// "haze" or "hero_haze", its id as an int or uint32, or AnyHero for the
	// hero the player has or else a random one. Nil is AnyHero.
	Hero any
	// Team is the team, 2 or 3. Zero keeps the player's hero's team or joins
	// the side with fewer heroes.
	Team int32
}

// wish is what one seated player wants, with the generation of the
// connection it was asked for, so a wish never passes to the next player in
// the slot.
type wish struct {
	// generation is the connection's generation when the mod seated it.
	generation uint32
	// want is the seat with the respawn delay.
	want Want
}

// seats keeps each player the mod seats on the hero and team it asked for,
// and brings fallen heroes back after the respawn delay, through one Cast it
// runs with the mod's frames.
type seats struct {
	// wishes holds each seated player's wish by slot.
	wishes map[int32]*wish
	// respawn is how long a fallen hero waits, or zero for the game's own
	// time.
	respawn time.Duration
	// cast seats the players, once the mod uses seating.
	cast *Cast[*Connection]
}

// seating is the mod's seats.
var seating = &seats{wishes: map[int32]*wish{}}

// held returns the wish of the cast's member, or nil when the mod did not
// seat them.
func (s *seats) held(member *Connection) *wish {
	if held := s.wishes[member.GetPlayer()]; held != nil && held.generation == member.GetGeneration() {
		return held
	}
	return nil
}

// start makes the Cast and runs it with the mod's frames. Bots are members,
// so the mod can seat one and the respawn delay brings theirs back too. It
// starts with the first seat or respawn delay, so a mod that uses neither
// pays nothing.
func (s *seats) start() {
	if s.cast != nil {
		return
	}
	s.cast = NewCast(CastOptions[*Connection]{
		Join: func(connection *Connection) *Connection { return connection },
		Leave: func(member *Connection, _ *Connection) {
			if s.held(member) != nil {
				delete(s.wishes, member.GetPlayer())
			}
		},
		WantOf: func(member *Connection) Want {
			if held := s.held(member); held != nil {
				return held.want
			}
			return Want{Respawn: s.respawn}
		},
		Bots: BotsJoin,
	})
	OnFrame(func(frame *FrameEvent) {
		s.cast.Frame(frame.GetTimeSeconds())
	})
}

// seat records what the player behind held wants and starts the Cast.
func (s *seats) seat(held *Connection, want Want) {
	want.Respawn = s.respawn
	s.wishes[held.GetPlayer()] = &wish{generation: held.GetGeneration(), want: want}
	s.start()
}

// delay sets how long every fallen hero waits and starts the Cast.
func (s *seats) delay(delay time.Duration) {
	s.respawn = delay
	for _, held := range s.wishes {
		held.want.Respawn = delay
	}
	s.start()
}

// Seat gives the player a hero on a team, skipping the game's team and hero
// screens. Call it as the player joins, in an OnPlayerJoin handler.
//
// The hero appears on a later frame, and OnSpawn hears of it. The library
// asks the game again until the player has the seat, and once more after the
// player changes hero, so a call before the game is ready is not lost.
// Seating the player again replaces the seat, and it ends when they leave.
// It returns false when nobody holds the player's slot.
func (p Player) Seat(seat Seating) bool {
	// Find the connection the seat is for.
	held := p.Connection()
	if held == nil {
		return false
	}

	// Without a hero, any hero does.
	hero := seat.Hero
	if hero == nil {
		hero = AnyHero
	}
	seating.seat(held, Want{Hero: hero, Team: seat.Team})
	return true
}

// SetRespawnDelay brings every fallen hero back after delay, in place of the
// game's own respawn time. Zero leaves fallen heroes to the game again.
func SetRespawnDelay(delay time.Duration) {
	seating.delay(delay)
}
