// Command race is a checkpoint race in Go. Players mark a course by standing
// on each checkpoint and typing /mark; the first mark is the start line. /race
// lines every player up at the start, counts down and times each lap through
// the checkpoints in order and back to the start line. Racers take no damage,
// a fallen racer or one who presses their reset key returns to the last
// checkpoint passed, and the first finish gives the others thirty seconds.
// Each player's best time on the course is kept until the course changes.
//
// Commands: /mark, /unmark, /clear, /race [laps], /stop and /best. Another
// mod reads the last race's standings from the race service.
package main

import "github.com/paralin/modlock/mod"

// init registers the mode's handlers before the server starts the mod.
func init() {
	// Prepare the reset keys, and start over on each new world.
	m := NewMode()
	mod.OnStart(m.Started)
	mod.OnWorld(m.World)

	// Run the race on the game clock and the racers' input and hits.
	mod.OnFrame(m.Frame)
	mod.OnInput(m.Input)
	mod.OnDamage(m.Damage)
	mod.OnSettingChanged(m.SettingChanged)

	// Answer the course and race commands.
	mod.Command("mark", m.Mark)
	mod.Command("unmark", m.Unmark)
	mod.Command("clear", m.ClearCourse)
	mod.Command("race", m.StartRace)
	mod.Command("stop", m.StopRace)
	mod.Command("best", m.Best)

	// Serve the last race's standings to other mods.
	mod.Serve("race", m.Serve)
}

// main is required by Go and never runs; the server calls the handlers.
func main() {}
