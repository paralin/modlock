package main

import "github.com/paralin/modlock/mod"

// Racer is one player's progress through a race.
type Racer struct {
	// Player is the racing player.
	Player mod.Player
	// Generation tells the player apart from a later one in the same slot.
	Generation uint32
	// SteamID keys the player's best time.
	SteamID uint64
	// Name is the player's name in the standings.
	Name string
	// Entity is the racer's hero, as damage events name it.
	Entity uint32
	// Next is the index of the checkpoint the racer heads for; zero is the
	// start line.
	Next int
	// Lap counts the laps the racer completed.
	Lap int
	// Passed counts every checkpoint the racer passed, start lines included,
	// to rank those still racing.
	Passed int
	// Fallen marks a racer whose hero died, to return it to its last
	// checkpoint once it lives again.
	Fallen bool
	// Time is the race clock when the racer finished, in seconds.
	Time float64
	// Place is the racer's finishing place from one, or zero while racing.
	Place int
}
