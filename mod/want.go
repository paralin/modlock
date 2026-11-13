package mod

import "time"

// AnyHero is the Want.Hero that keeps the hero a member has, or else seats
// them on a random released hero.
const AnyHero = "any"

// Want is what a member's hero should be; each field it leaves at its zero
// value is free.
type Want struct {
	// Hero seats the member on a hero: its game name, such as "hero_atlas",
	// its id as an int or uint32, or AnyHero for the hero they have or else a
	// random one. Nil leaves the hero free.
	Hero any
	// Team is the team, 2 or 3, to seat the member on. Without one a member
	// keeps their hero's team, or else joins the side with fewer heroes.
	Team int32
	// Respawn is how long a fallen hero waits before Cast asks for it back.
	// Zero leaves a fallen hero to the game.
	Respawn time.Duration
}
