package main

import (
	"slices"

	"github.com/paralin/modlock/mod"
)

// reach is how close, in units, a hero must come to a checkpoint to pass it.
const reach = 180

// grace is how long, in seconds, the others race on after the first finish.
const grace = 30

// Pass is what one move did to a racer's progress.
type Pass int

const (
	// PassNone leaves the racer's progress as it was.
	PassNone Pass = iota
	// PassCheckpoint passes the racer's next checkpoint.
	PassCheckpoint
	// PassLap crosses the start line and begins another lap.
	PassLap
	// PassFinish crosses the start line at the end of the last lap.
	PassFinish
)

// Race is one run of a course: its racers, the start and the finish. A lap
// runs through every checkpoint in order and back to the start line. Race
// makes no calls into the game, so its rules test anywhere.
type Race struct {
	// checkpoints are the course's positions; the first is the start line.
	checkpoints []*mod.Vector
	// laps is how many laps finish the race.
	laps int
	// startsAt is the game time the countdown ends, in seconds.
	startsAt float64
	// closesAt is the game time the race ends for those still racing, or
	// zero before anyone finishes.
	closesAt float64
	// racers are the race's racers in join order.
	racers []*Racer
	// finished counts the racers who finished.
	finished int
}

// NewRace returns a race of laps over checkpoints, at least two, whose
// countdown ends at startsAt.
func NewRace(checkpoints []*mod.Vector, laps int, startsAt float64) *Race {
	return &Race{checkpoints: slices.Clone(checkpoints), laps: max(laps, 1), startsAt: startsAt}
}

// Laps returns how many laps finish the race.
func (r *Race) Laps() int {
	return r.laps
}

// Checkpoints returns how many checkpoints a lap has, the start line
// included.
func (r *Race) Checkpoints() int {
	return len(r.checkpoints)
}

// Join adds a racer at the start line, heading for the first checkpoint.
func (r *Race) Join(racer *Racer) {
	racer.Next = 1
	r.racers = append(r.racers, racer)
}

// Drop removes a racer still racing, such as one who left the server. A
// finisher keeps their place in the standings.
func (r *Race) Drop(racer *Racer) {
	if racer.Place == 0 {
		r.racers = slices.DeleteFunc(r.racers, func(other *Racer) bool { return other == racer })
	}
}

// Racers returns the race's racers in join order.
func (r *Race) Racers() []*Racer {
	return r.racers
}

// Find returns the player's racer, or nil when they are not racing.
func (r *Race) Find(p mod.Player) *Racer {
	for _, racer := range r.racers {
		if racer.Player == p {
			return racer
		}
	}
	return nil
}

// Clock returns the race clock at the game time now, in seconds. It is
// negative during the countdown.
func (r *Race) Clock(now float64) float64 {
	return now - r.startsAt
}

// Over reports whether the race has ended at now: everyone finished, or the
// grace after the first finish ran out.
func (r *Race) Over(now float64) bool {
	return r.finished == len(r.racers) || r.closesAt != 0 && now >= r.closesAt
}

// Track moves racer's progress for its hero standing at position at the game
// time now, and reports what the move passed.
func (r *Race) Track(racer *Racer, position *mod.Vector, now float64) Pass {
	// Hold everyone during the countdown, and anyone who finished or has
	// not reached their next checkpoint.
	if now < r.startsAt || racer.Place != 0 || distance(position, r.checkpoints[racer.Next]) > reach {
		return PassNone
	}
	racer.Passed++

	// Head for the following checkpoint, or back to the start line after the
	// last one.
	if racer.Next != 0 {
		racer.Next = (racer.Next + 1) % len(r.checkpoints)
		return PassCheckpoint
	}

	// Cross the start line into another lap until the last one.
	racer.Lap++
	racer.Next = 1
	if racer.Lap < r.laps {
		return PassLap
	}

	// Finish, and give the others the grace from the first finish.
	r.finished++
	racer.Place = r.finished
	racer.Time = r.Clock(now)
	if r.closesAt == 0 {
		r.closesAt = now + grace
	}
	return PassFinish
}

// Last returns the checkpoint racer passed last, where a reset returns it.
func (r *Race) Last(racer *Racer) *mod.Vector {
	return r.checkpoints[(racer.Next+len(r.checkpoints)-1)%len(r.checkpoints)]
}

// Ahead returns the checkpoint racer heads for.
func (r *Race) Ahead(racer *Racer) *mod.Vector {
	return r.checkpoints[racer.Next]
}

// Standings returns the racers best first: finishers by place, then those
// still racing by the checkpoints they passed.
func (r *Race) Standings() []*Racer {
	standings := slices.Clone(r.racers)
	slices.SortStableFunc(standings, func(a, b *Racer) int {
		switch {
		case a.Place != 0 && b.Place != 0:
			return a.Place - b.Place
		case a.Place != 0:
			return -1
		case b.Place != 0:
			return 1
		}
		return b.Passed - a.Passed
	})
	return standings
}
