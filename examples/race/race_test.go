package main

import (
	"slices"
	"testing"

	"github.com/paralin/modlock/mod"
)

// square is a course of a start line and three checkpoints at the corners of
// a square far wider than reach.
var square = []*mod.Vector{{X: 0}, {X: 1000}, {X: 1000, Y: 1000}, {Y: 1000}}

// TestRaceLaps checks that a racer passes the checkpoints only in order and
// finishes on the start line after the last lap.
func TestRaceLaps(t *testing.T) {
	// Race one racer over two laps of the square.
	race := NewRace(square, 2, 10)
	racer := &Racer{Name: "a"}
	race.Join(racer)

	// The countdown holds the racer even on a checkpoint.
	if pass := race.Track(racer, square[1], 9); pass != PassNone {
		t.Fatalf("countdown: got %v, want PassNone", pass)
	}

	// Skipping ahead passes nothing; each checkpoint in order passes.
	if pass := race.Track(racer, square[2], 11); pass != PassNone {
		t.Fatalf("skip: got %v, want PassNone", pass)
	}
	var passes []Pass
	for lap := range 2 {
		for _, i := range []int{1, 2, 3, 0} {
			passes = append(passes, race.Track(racer, square[i], 20+float64(lap*10+i)))
		}
	}
	want := []Pass{
		PassCheckpoint, PassCheckpoint, PassCheckpoint, PassLap,
		PassCheckpoint, PassCheckpoint, PassCheckpoint, PassFinish,
	}
	if !slices.Equal(passes, want) {
		t.Fatalf("passes: got %v, want %v", passes, want)
	}

	// The finish stops the clock at the last crossing and ends the race.
	if racer.Place != 1 || racer.Time != 20 || !race.Over(30) {
		t.Fatalf("finish: place %d, time %v, over %v", racer.Place, racer.Time, race.Over(30))
	}
}

// TestRaceGrace checks that the first finish gives the others grace before
// the race ends, and that the standings rank finishers before the rest.
func TestRaceGrace(t *testing.T) {
	// Race three racers over one lap of a start line and one checkpoint.
	race := NewRace(square[:2], 1, 0)
	fast, slow, idle := &Racer{Name: "fast"}, &Racer{Name: "slow"}, &Racer{Name: "idle"}
	race.Join(idle)
	race.Join(slow)
	race.Join(fast)

	// The fast racer laps; the slow one passes one checkpoint.
	race.Track(fast, square[1], 1)
	race.Track(fast, square[0], 2)
	race.Track(slow, square[1], 3)

	// The others race on until the grace runs out.
	if race.Over(2+grace-1) || !race.Over(2+grace) {
		t.Fatal("the race should end exactly when the grace runs out")
	}
	got := race.Standings()
	if !slices.Equal(got, []*Racer{fast, slow, idle}) {
		t.Fatalf("standings: got %s, %s, %s", got[0].Name, got[1].Name, got[2].Name)
	}
}

// TestRaceResetAndDrop checks where a reset returns a racer and that a racer
// who leaves stops holding the race open.
func TestRaceResetAndDrop(t *testing.T) {
	// Race two racers over one lap of the square.
	race := NewRace(square, 1, 0)
	stays, leaves := &Racer{Player: mod.Player{Slot: 1}}, &Racer{Player: mod.Player{Slot: 2}}
	race.Join(stays)
	race.Join(leaves)

	// A new racer resets to the start line; one past the last checkpoint
	// resets there, facing the start line.
	if race.Last(stays) != square[0] || race.Ahead(stays) != square[1] {
		t.Fatal("a new racer should reset to the start line")
	}
	for _, i := range []int{1, 2, 3} {
		race.Track(stays, square[i], 1)
	}
	if race.Last(stays) != square[3] || race.Ahead(stays) != square[0] {
		t.Fatal("a racer should reset to the checkpoint passed last")
	}

	// Once the leaver drops, the stayer's finish ends the race.
	race.Drop(leaves)
	race.Track(stays, square[0], 2)
	if !race.Over(2) || race.Find(leaves.Player) != nil {
		t.Fatal("a dropped racer should neither hold the race open nor stay in it")
	}
}
