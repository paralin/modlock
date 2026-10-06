package main

import (
	"errors"
	"math"
	"strconv"
	"strings"

	"github.com/paralin/modlock/mod"
)

// countdown is how long the racers wait at the start line, in seconds.
const countdown = 5

// Mode is the race mode on one server: the course players marked, the race
// running over it and each player's best time. Its methods are the mod's
// event and command handlers, which the server calls one at a time.
type Mode struct {
	// course is the checkpoints players marked on this world.
	course *Course
	// race is the race running now, or nil between races.
	race *Race
	// now is the game time of the latest frame, in seconds.
	now float64
	// bests maps each player's Steam ID to their best time on the course, in
	// seconds.
	bests map[uint64]float64
	// results is the last race's standings, which the race service returns.
	results string
}

// NewMode returns a mode with no course.
func NewMode() *Mode {
	return &Mode{course: &Course{}, bests: map[uint64]float64{}}
}

// Started watches the keys a racer may choose to reset with.
func (m *Mode) Started([]string) {
	if err := mod.WatchInput(mod.ButtonReload | mod.ButtonUse); err != nil {
		mod.Log("race: cannot watch the reset keys: ", err)
	}
}

// World starts over on a new world, which took the course's labels with it.
func (m *Mode) World(string) {
	m.course.Forget()
	m.race = nil
	clear(m.bests)
}

// Mark adds a checkpoint where the player's hero stands.
func (m *Mode) Mark(p mod.Player, _ string) {
	// Keep the course as it is while a race runs over it.
	if m.race != nil {
		_ = p.Chat("The course stays as it is during a race.")
		return
	}

	// Mark the spot under the player's living hero.
	pawn, err := p.Pawn()
	if err != nil || pawn == nil || pawn.GetHealth() <= 0 {
		_ = p.Chat("You need a living hero to mark a checkpoint.")
		return
	}
	m.course.Add(pawn.GetPosition())
	clear(m.bests)

	// Say what the mark made and what comes next.
	if m.course.Len() == 1 {
		_ = p.Chat("Start line marked. Walk the course and /mark each checkpoint.")
		return
	}
	_ = p.Chat("Checkpoint " + strconv.Itoa(m.course.Len()-1) + " marked. /race starts a race.")
}

// Unmark removes the newest checkpoint.
func (m *Mode) Unmark(p mod.Player, _ string) {
	// Keep the course as it is while a race runs over it.
	if m.race != nil {
		_ = p.Chat("The course stays as it is during a race.")
		return
	}

	// Remove the checkpoint, which changes every best time.
	if !m.course.RemoveLast() {
		_ = p.Chat("The course has no checkpoints.")
		return
	}
	clear(m.bests)
	_ = p.Chat("Removed the last checkpoint.")
}

// ClearCourse removes every checkpoint, and the best times they set.
func (m *Mode) ClearCourse(p mod.Player, _ string) {
	if m.race != nil {
		_ = p.Chat("The course stays as it is during a race.")
		return
	}
	m.course.Clear()
	clear(m.bests)
	broadcast("The course is cleared. /mark a new start line.")
}

// StartRace lines every player with a living hero up at the start line and
// starts the countdown. args may name the laps; the starter's laps setting
// decides otherwise.
func (m *Mode) StartRace(p mod.Player, args string) {
	// Refuse a second race and a course too short to lap.
	if m.race != nil {
		_ = p.Chat("A race is already running. /stop ends it.")
		return
	}
	if m.course.Len() < 2 {
		_ = p.Chat("Mark a start line and at least one checkpoint with /mark first.")
		return
	}

	// Read the laps from the command, else from the starter's setting.
	laps, err := strconv.Atoi(args)
	if err != nil {
		chosen, _ := p.SettingNumber("laps")
		laps = int(chosen)
	}

	// Line the racers up at the start line and hold them there.
	m.race = NewRace(m.course.Checkpoints(), laps, m.now+countdown)
	connections, _ := mod.Players()
	for _, c := range connections {
		player := mod.Player{Slot: c.GetPlayer()}
		pawn, err := player.Pawn()
		if c.GetBot() || err != nil || pawn == nil || pawn.GetHealth() <= 0 {
			continue
		}
		racer := &Racer{
			Player:     player,
			Generation: c.GetGeneration(),
			SteamID:    c.GetSteamId(),
			Name:       c.GetName(),
			Entity:     pawn.GetEntity(),
		}
		m.race.Join(racer)
		m.reset(racer)
		_ = player.Freeze(nil)
		_ = player.CenterText(strconv.Itoa(countdown))
	}

	// Call the race off when nobody could line up.
	if len(m.race.Racers()) == 0 {
		m.race = nil
		_ = p.Chat("Nobody has a living hero to race.")
		return
	}
	broadcast("A race of " + plural(m.race.Laps(), "lap") + " starts in " + strconv.Itoa(countdown) + " seconds.")
}

// StopRace ends the running race early, with the standings so far.
func (m *Mode) StopRace(p mod.Player, _ string) {
	if m.race == nil {
		_ = p.Chat("No race is running.")
		return
	}
	m.finish()
}

// Best tells the player their best time on the course.
func (m *Mode) Best(p mod.Player, _ string) {
	// Find the player's Steam ID, which keys their best.
	connections, _ := mod.Players()
	var steamID uint64
	for _, c := range connections {
		if c.GetPlayer() == p.Slot {
			steamID = c.GetSteamId()
		}
	}

	// Report the best, or how to set one.
	time, ok := m.bests[steamID]
	if !ok {
		_ = p.Chat("You have no time on this course yet. /race starts a race.")
		return
	}
	_ = p.Chat("Your best on this course is " + clockText(time, 1) + ".")
}

// Serve answers the race service: standings returns the last race's
// standings, one racer per line.
func (m *Mode) Serve(method string, _ []byte) ([]byte, error) {
	if method != "standings" {
		return nil, errors.New("the race service has no method " + method)
	}
	return []byte(m.results), nil
}

// broadcast sends a chat line to every player who is not a bot.
func broadcast(text string) {
	connections, _ := mod.Players()
	for _, c := range connections {
		if !c.GetBot() {
			_ = mod.Player{Slot: c.GetPlayer()}.Chat(text)
		}
	}
}

// Frame runs the race on each server frame: the countdown, then each racer's
// progress, then the finish.
func (m *Mode) Frame(f *mod.FrameEvent) {
	// Advance the game clock; nothing more runs between races.
	previous := m.now
	m.now = f.GetTimeSeconds()
	if m.race == nil {
		return
	}

	// Count down once a second, then release the racers.
	clock, before := m.race.Clock(m.now), m.race.Clock(previous)
	tick := math.Floor(clock) != math.Floor(before)
	if clock < 0 {
		if tick {
			m.everyRacer(func(racer *Racer) { _ = racer.Player.CenterText(strconv.Itoa(int(-math.Floor(clock)))) })
		}
		return
	}
	if before < 0 {
		m.release()
	}

	// Track the racers still on the server.
	m.dropLeavers()
	for _, racer := range m.race.Racers() {
		m.track(racer, tick)
	}

	// End the race once everyone finished or the grace ran out.
	if m.race.Over(m.now) {
		m.finish()
	}
}

// release lets the racers go when the countdown ends.
func (m *Mode) release() {
	held := false
	m.everyRacer(func(racer *Racer) {
		_ = racer.Player.Freeze(&held)
		_ = racer.Player.CenterText("Go!")
	})
}

// dropLeavers drops the racers whose players left, so the race does not wait
// for them. A new player in a racer's slot has another generation.
func (m *Mode) dropLeavers() {
	// Read who is on the server; without an answer, keep everyone.
	connections, err := mod.Players()
	if err != nil {
		return
	}
	present := map[int32]uint32{}
	for _, c := range connections {
		present[c.GetPlayer()] = c.GetGeneration()
	}

	// Drop each racer whose player is gone.
	var left []*Racer
	for _, racer := range m.race.Racers() {
		if generation, ok := present[racer.Player.Slot]; !ok || generation != racer.Generation {
			left = append(left, racer)
		}
	}
	for _, racer := range left {
		m.race.Drop(racer)
	}
}

// track moves one racer along the course and shows them what changed. tick
// redraws their clock once a second.
func (m *Mode) track(racer *Racer, tick bool) {
	// Skip a finisher and a racer whose hero is between lives.
	if racer.Place != 0 {
		return
	}
	pawn, err := racer.Player.Pawn()
	if err != nil || pawn == nil {
		return
	}
	racer.Entity = pawn.GetEntity()

	// Revive a fallen hero, and return it to its last checkpoint once it
	// lives again.
	if pawn.GetHealth() <= 0 {
		racer.Fallen = true
		_ = racer.Player.Respawn()
		return
	}
	if racer.Fallen {
		racer.Fallen = false
		m.reset(racer)
	}

	// Move the racer along the course and mark what they passed.
	switch m.race.Track(racer, pawn.GetPosition(), m.now) {
	case PassCheckpoint:
		m.split(racer)
	case PassLap:
		_ = racer.Player.Announce("Lap "+strconv.Itoa(racer.Lap+1), "of "+strconv.Itoa(m.race.Laps()))
	case PassFinish:
		m.finished(racer)
		return
	case PassNone:
		if !tick {
			return
		}
	}
	m.show(racer)
}

// split chimes for a passed checkpoint and, when the racer asked for splits,
// writes its time in their chat.
func (m *Mode) split(racer *Racer) {
	// Chime and count the checkpoint.
	_ = racer.Player.Sound("Damage.Send.Crit")
	_ = racer.Player.AddMetric("race.checkpoints", 1, nil)

	// Write the split, numbered as the checkpoint's label.
	if on, err := racer.Player.SettingOn("splits"); err != nil || !on {
		return
	}
	passed := (racer.Next + m.race.Checkpoints() - 1) % m.race.Checkpoints()
	_ = racer.Player.Chat("Checkpoint " + strconv.Itoa(passed) + ": " + clockText(m.race.Clock(m.now), 1))
}

// show writes the racer's lap, next checkpoint and clock in the middle of
// their screen.
func (m *Mode) show(racer *Racer) {
	// Name where the racer heads: a checkpoint, the start line or the finish.
	next := "checkpoint " + strconv.Itoa(racer.Next)
	if racer.Next == 0 {
		next = "the start line"
		if racer.Lap+1 == m.race.Laps() {
			next = "the finish"
		}
	}

	// Show it with the lap and the whole seconds raced.
	lap := "Lap " + strconv.Itoa(racer.Lap+1) + " of " + strconv.Itoa(m.race.Laps())
	_ = racer.Player.CenterText(lap + " · Next: " + next + " · " + clockText(m.race.Clock(m.now), 0))
}

// finished congratulates a racer who crossed the finish and keeps their
// score.
func (m *Mode) finished(racer *Racer) {
	// Clear the racer's clock and announce the place to everyone.
	time := clockText(racer.Time, 1)
	_ = racer.Player.CenterText("")
	_ = racer.Player.Announce(ordinal(racer.Place)+" place", time)
	broadcast(racer.Name + " finished " + ordinal(racer.Place) + " in " + time + ".")

	// Count the finish, its time and a win.
	_ = racer.Player.AddMetric("race.finished", 1, nil)
	_ = racer.Player.AddMetric("race.seconds", racer.Time, nil)
	if racer.Place == 1 {
		_ = racer.Player.AddMetric("race.won", 1, nil)
	}

	// Keep a new best, and say by how much it beat the old one.
	previous, ok := m.bests[racer.SteamID]
	if ok && previous <= racer.Time {
		return
	}
	m.bests[racer.SteamID] = racer.Time
	if ok {
		_ = racer.Player.Chat("A new best, " + clockText(previous-racer.Time, 1) + " faster.")
	}
}

// finish ends the race: it releases anyone still held, clears the racers'
// screens and posts the standings.
func (m *Mode) finish() {
	// Write the standings: finishers with their times, then the rest.
	var lines []string
	for _, racer := range m.race.Standings() {
		if racer.Place == 0 {
			lines = append(lines, racer.Name+" did not finish")
			continue
		}
		lines = append(lines, ordinal(racer.Place)+"  "+racer.Name+"  "+clockText(racer.Time, 1))
	}
	m.results = strings.Join(lines, "\n")

	// Release and clear every racer, then end the race.
	held := false
	m.everyRacer(func(racer *Racer) {
		_ = racer.Player.Freeze(&held)
		_ = racer.Player.CenterText("")
	})
	m.race = nil

	// Post the standings.
	broadcast("The race is over.")
	for _, line := range lines {
		broadcast(line)
	}
}

// reset returns a racer's hero to its last checkpoint, still and facing the
// next one, with full stamina.
func (m *Mode) reset(racer *Racer) {
	last := m.race.Last(racer)
	_ = racer.Player.Teleport(last, toward(last, m.race.Ahead(racer)), &mod.Vector{})
	_ = racer.Player.RestoreStamina()
}

// Input resets a racer who pressed the reset key they chose.
func (m *Mode) Input(p mod.Player, pressed, _ uint64) {
	// Only a racer on the course after the countdown resets.
	if m.race == nil || m.race.Clock(m.now) < 0 {
		return
	}
	racer := m.race.Find(p)
	if racer == nil || racer.Place != 0 {
		return
	}

	// Reset on the player's key.
	if pressed&resetButton(p) != 0 {
		m.reset(racer)
	}
}

// resetButton returns the button the player's reset setting chose, or zero
// when they turned resets off.
func resetButton(p mod.Player) uint64 {
	setting, _ := p.Setting("reset")
	switch setting {
	case "use":
		return mod.ButtonUse
	case "off":
		return 0
	}
	return mod.ButtonReload
}

// Damage blocks every hit on a racer's hero during a race.
func (m *Mode) Damage(hit *mod.DamageEvent) *mod.DamageResult {
	if m.race == nil {
		return nil
	}
	for _, racer := range m.race.Racers() {
		if racer.Entity == hit.GetVictim() {
			block := true
			return &mod.DamageResult{Block: &block}
		}
	}
	return nil
}

// SettingChanged confirms a new reset key the player chose on their
// profile.
func (m *Mode) SettingChanged(p mod.Player, key, value string) {
	if key != "reset" {
		return
	}
	switch value {
	case "use":
		_ = p.Chat("Use now returns you to your last checkpoint.")
	case "off":
		_ = p.Chat("Returning to your last checkpoint is off.")
	default:
		_ = p.Chat("Reload now returns you to your last checkpoint.")
	}
}

// everyRacer runs do for each racer in the race.
func (m *Mode) everyRacer(do func(racer *Racer)) {
	for _, racer := range m.race.Racers() {
		do(racer)
	}
}
