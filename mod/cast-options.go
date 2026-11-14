package mod

// CastOptions are a mode's answers to its players coming and going.
type CastOptions[M any] struct {
	// Join makes the member for a player who finished joining. It is required.
	Join func(connection *Connection) M
	// Leave ends a member whose connection left the slot, or nil.
	Leave func(member M, connection *Connection)
	// Want is what every member wants. WantOf replaces it when set.
	Want Want
	// WantOf returns what one member wants now; nil uses Want. Cast calls it
	// for each member on most frames, so a want follows the member's own state
	// and the function should be cheap.
	WantOf func(member M) Want
	// Bots says what Cast does with a bot; BotsSkip is the default.
	Bots BotPolicy
}

// BotPolicy is what a Cast does with a bot.
type BotPolicy int

const (
	// BotsSkip leaves bots out of the cast.
	BotsSkip BotPolicy = iota
	// BotsJoin makes a bot a member as a human is.
	BotsJoin
	// BotsKick asks the server to remove each bot, once per connection.
	BotsKick
)
