package mod

import (
	"math"
	"math/rand/v2"
)

// changeSeconds is how long Cast leaves a member without a hero after they had
// one, or after they joined when their hero is free. The game's own Change
// Hero removes the hero before it spawns the new one, and seating the member
// in that gap would override their pick.
const changeSeconds = 3

// Cast keeps a mode's players: one member of type M for each human who
// finished joining, made by the mode's Join and ended by its Leave. Members are
// keyed by slot and connection generation, so a reconnect is a leave and a
// join. Cast learns of joins and leaves from the host's player reports and
// settles them on the next Frame; it never walks the player list on a frame
// where nothing changed.
//
// A member may carry a Want: to sit on a hero and team, and to come back a
// while after its hero falls. A human whose want leaves the hero free still
// gets one when the game leaves them on a team without a hero, as its own
// spawn on joining can, since nothing else would ever seat them. Each frame
// Cast compares the want with what the member's hero shows, asks the game
// again at most once a second while they differ, and stops asking once they
// match. That is a reconciliation loop: a lost or refused ask is only asked
// again, never reported as a failure.
//
// A frame costs one pawn read per member, which the mode reads back with Pawn
// instead of asking the game again.
type Cast[M any] struct {
	// options are the mode's answers to players coming and going.
	options CastOptions[M]
	// seats holds one seat per member in the order they joined.
	seats []*seat[M]
	// kicked holds the connection generation each kicked bot's slot had.
	kicked map[int32]uint32
	// rosters is the count of player lists the host had reported when Cast
	// last settled one.
	rosters uint64
}

// seat is one member with what Cast last saw of their hero.
type seat[M any] struct {
	// connection is the member's connection as it joined.
	connection *Connection
	// player addresses the member's slot.
	player Player
	// member is what the mode's Join made.
	member M
	// pawn is the member's hero this frame, or nil.
	pawn *Pawn
	// alive reports whether the hero was standing this frame.
	alive bool
	// fellAt is when Cast last saw the hero fall.
	fellAt float64
	// hero is the member's hero last frame, or 0 without one.
	hero uint32
	// heroLostAt is when the member last went from a hero to none.
	heroLostAt float64
	// joinedAt is when the member joined.
	joinedAt float64
	// askedAt is when Cast last asked the game for the want.
	askedAt float64
}

// NewCast returns a Cast that answers players coming and going with options.
// It settles the player list the host last reported on its first Frame.
func NewCast[M any](options CastOptions[M]) *Cast[M] {
	return &Cast[M]{options: options, kicked: map[int32]uint32{}}
}

// Size returns the number of members.
func (c *Cast[M]) Size() int {
	return len(c.seats)
}

// Members lists the members in the order they joined.
func (c *Cast[M]) Members() []M {
	members := make([]M, 0, len(c.seats))
	for _, seat := range c.seats {
		members = append(members, seat.member)
	}
	return members
}

// Get returns the member in the player's slot, and false when there is none.
func (c *Cast[M]) Get(p Player) (M, bool) {
	if seat := c.find(p); seat != nil {
		return seat.member, true
	}
	var none M
	return none, false
}

// Pawn returns the member's hero as Cast read it this frame, or nil.
func (c *Cast[M]) Pawn(p Player) *Pawn {
	if seat := c.find(p); seat != nil {
		return seat.pawn
	}
	return nil
}

// Frame settles the joins and leaves reported since the last frame, reads
// each member's hero, and asks the game once a second for any want it does not
// show yet. seconds is the game clock, as FrameEvent.TimeSeconds reports it.
func (c *Cast[M]) Frame(seconds float64) {
	// Settle a player list the host reported since the last frame.
	if c.rosters != registered.rosters {
		c.rosters = registered.rosters
		c.settle(registered.roster, seconds)
	}

	// Reconcile each member's hero with their want.
	for _, seat := range c.seats {
		c.reconcile(seat, seconds)
	}
}

// find returns the seat in the player's slot, or nil.
func (c *Cast[M]) find(p Player) *seat[M] {
	for _, seat := range c.seats {
		if seat.player == p {
			return seat
		}
	}
	return nil
}

// settle ends the members whose connection left and makes the new ones, joined
// at now.
func (c *Cast[M]) settle(players []*Connection, now float64) {
	// A member leaves when their slot empties or a new connection takes it.
	present := make(map[int32]uint32, len(players))
	for _, connection := range players {
		present[connection.GetPlayer()] = connection.GetGeneration()
	}
	kept := c.seats[:0]
	var left []*seat[M]
	for _, seat := range c.seats {
		generation, ok := present[seat.player.Slot()]
		if ok && generation == seat.connection.GetGeneration() {
			kept = append(kept, seat)
			continue
		}
		left = append(left, seat)
	}
	clear(c.seats[len(kept):])
	c.seats = kept

	// End each member who left.
	if c.options.Leave != nil {
		for _, seat := range left {
			c.options.Leave(seat.member, seat.connection)
		}
	}

	// A human who finished joining becomes a member, and a bot as the policy
	// says.
	for _, connection := range players {
		player := PlayerOf(connection.GetPlayer())
		if !connection.GetReady() || c.find(player) != nil {
			continue
		}
		if connection.GetBot() && c.options.Bots != BotsJoin {
			if c.options.Bots == BotsKick {
				c.kick(connection)
			}
			continue
		}
		c.seats = append(c.seats, &seat[M]{
			connection: connection,
			player:     player,
			member:     c.options.Join(connection),
			fellAt:     math.Inf(-1),
			heroLostAt: math.Inf(-1),
			joinedAt:   now,
			askedAt:    math.Inf(-1),
		})
	}
}

// kick asks the server to remove a bot, once per connection.
func (c *Cast[M]) kick(connection *Connection) {
	// Remember the connection so a later report does not kick it again.
	slot := connection.GetPlayer()
	if generation, ok := c.kicked[slot]; ok && generation == connection.GetGeneration() {
		return
	}
	c.kicked[slot] = connection.GetGeneration()

	// Ask the server console to remove the bot by name.
	if err := ServerCommand(`kick "` + connection.GetName() + `"`); err != nil {
		Log("bot ", connection.GetName(), " not kicked: ", err)
	}
}

// reconcile reads a member's hero and asks for the part of the want it lacks.
func (c *Cast[M]) reconcile(seat *seat[M], now float64) {
	// Note when the hero falls. A failed read counts as no pawn this frame.
	pawn, err := seat.player.Pawn()
	if err != nil {
		pawn = nil
	}
	alive := pawn.GetHero() != 0 && pawn.GetHealth() > 0
	if seat.alive && !alive {
		seat.fellAt = now
	}
	seat.pawn = pawn
	seat.alive = alive

	// Note when a member who had a hero is left without one, so a hero change
	// the game is making finishes before Cast seats them again.
	had := pawn.GetHero()
	if had == 0 && seat.hero != 0 {
		seat.heroLostAt = now
	}
	seat.hero = had
	changing := had == 0 && now-seat.heroLostAt < changeSeconds

	// Ask at most once a second, first for the seat, then for the hero back.
	if now-seat.askedAt < 1 || changing {
		return
	}
	want := c.wantOf(seat)
	hero := heroOf(want.Hero)
	if hero == nil {
		hero = c.stranded(seat, now)
	}
	switch {
	case hero != nil && !c.seated(seat, want, hero):
		seat.askedAt = now
		c.seat(seat, want, hero)
	case want.Respawn > 0 && had != 0 && !alive && now-seat.fellAt >= want.Respawn.Seconds():
		seat.askedAt = now
		_ = seat.player.Respawn()
	}
}

// stranded returns AnyHero for a human whose hero is free when the game has
// left them on a team without one for changeSeconds since they joined, and
// otherwise nil, leaving the hero free. Such a pawn never gets a hero on its
// own, while a player still choosing one has no pawn and a spectator stands on
// team 1. A bot is left to the game, which seats it.
func (c *Cast[M]) stranded(seat *seat[M], now float64) any {
	team := seat.pawn.GetTeam()
	heroless := seat.pawn != nil && seat.pawn.GetHero() == 0 && (team == 2 || team == 3)
	if heroless && !seat.connection.GetBot() && now-seat.joinedAt >= changeSeconds {
		return AnyHero
	}
	return nil
}

// wantOf returns what the member in seat wants now.
func (c *Cast[M]) wantOf(seat *seat[M]) Want {
	if c.options.WantOf != nil {
		return c.options.WantOf(seat.member)
	}
	return c.options.Want
}

// seated reports whether the member's hero shows the hero and team they want.
// hero is a hero's name, AnyHero or an id, as heroOf returns it.
func (c *Cast[M]) seated(seat *seat[M], want Want, hero any) bool {
	// A member needs a hero on the team they want before the hero counts.
	pawn := seat.pawn
	if pawn.GetHero() == 0 {
		return false
	}
	if want.Team != 0 && pawn.GetTeam() != want.Team {
		return false
	}

	// A name outside the released heroes, such as one the game added since, is
	// met by whatever hero the game gave for it.
	switch hero := hero.(type) {
	case uint32:
		return pawn.GetHero() == hero
	case string:
		known, ok := HeroNamed(hero)
		return hero == AnyHero || !ok || pawn.GetHero() == known.ID
	}
	return true
}

// seat asks the game to put the member on hero and their team. A refused ask
// is asked again on a later frame.
func (c *Cast[M]) seat(seat *seat[M], want Want, hero any) {
	// Any hero keeps the one the member has, or else takes a random one.
	if hero == AnyHero {
		hero = Heroes[rand.IntN(len(Heroes))].Name
		if had := seat.pawn.GetHero(); had != 0 {
			hero = had
		}
	}

	// Seat the member on the team they want, or else the side that needs them.
	team := want.Team
	if team == 0 {
		team = c.side(seat)
	}
	_, _ = seat.player.SelectHero(hero, team)
}

// side returns the team a member joins: their hero's, or the one with fewer
// heroes.
func (c *Cast[M]) side(seat *seat[M]) int32 {
	// A hero keeps its team. The game puts each connecting player on a team
	// before they have a hero, so a member without one is counted afresh.
	pawn := seat.pawn
	if pawn.GetHero() != 0 && (pawn.GetTeam() == 2 || pawn.GetTeam() == 3) {
		return pawn.GetTeam()
	}

	// Count the seated heroes on each side and join the smaller.
	var amber, sapphire int
	for _, other := range c.seats {
		if other == seat || other.pawn.GetHero() == 0 {
			continue
		}
		switch other.pawn.GetTeam() {
		case 2:
			amber++
		case 3:
			sapphire++
		}
	}
	if sapphire < amber {
		return 3
	}
	return 2
}

// heroOf returns a Want.Hero as SelectHero takes it: a hero's name or AnyHero
// as a string, or an id as a uint32. It returns nil for a free hero or a value
// of another type.
func heroOf(hero any) any {
	switch hero := hero.(type) {
	case string, uint32:
		return hero
	case int:
		return uint32(hero)
	case int32:
		return uint32(hero)
	}
	return nil
}
