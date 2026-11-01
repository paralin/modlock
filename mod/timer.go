package mod

import (
	"cmp"
	"fmt"
	"slices"
	"time"
)

// Timer is one scheduled callback. Time is the game's, so a paused game holds
// its timers. A tick is 1/64 second.
type Timer struct {
	finished bool
	// delay is the first wait in seconds, counted from the first frame when the
	// timer came before one.
	delay   float64
	started bool
	// at is the game time the timer next runs, once started.
	at float64
	// run returns the wait before the timer's next run, and false after its
	// last.
	run func() (time.Duration, bool)
}

// Finished reports whether the timer ran its last time or was cancelled.
func (t *Timer) Finished() bool {
	return t.finished
}

// Cancel stops the timer. It does nothing once the timer is finished.
func (t *Timer) Cancel() {
	t.finished = true
}

// clock holds the game time of the latest frame and the timers that have not
// finished, in creation order.
var clock struct {
	now    float64
	known  bool
	timers []*Timer
}

// After runs fn once, delay from now.
func After(delay time.Duration, fn func()) *Timer {
	return schedule(delay, func() (time.Duration, bool) {
		fn()
		return 0, false
	})
}

// Every runs fn each interval, keeping its cadence through a late frame.
func Every(interval time.Duration, fn func()) *Timer {
	return schedule(interval, func() (time.Duration, bool) {
		fn()
		return interval, true
	})
}

// Sequence calls fn on the next frame with the number of times it has run,
// starting at 1. fn returns the wait before it runs again and true, or false to
// end the sequence.
func Sequence(fn func(run int) (wait time.Duration, again bool)) *Timer {
	runs := 0
	return schedule(0, func() (time.Duration, bool) {
		runs++
		return fn(runs)
	})
}

// schedule adds a timer whose first run is delay away.
func schedule(delay time.Duration, run func() (time.Duration, bool)) *Timer {
	t := &Timer{delay: delay.Seconds(), started: clock.known, run: run}
	if clock.known {
		t.at = clock.now + t.delay
	}
	clock.timers = append(clock.timers, t)
	return t
}

// advanceTimers moves the clock to the frame at seconds and runs the timers that
// came due, at most once each and in the order they came due. A callback that
// panics ends only its own timer. A clock that runs backward, as a new map's
// does, keeps each timer's remaining delay.
func advanceTimers(seconds float64) {
	// Carry the timers across a clock that went back.
	if clock.known && seconds < clock.now {
		shift := seconds - clock.now
		for _, t := range clock.timers {
			if t.started {
				t.at += shift
			}
		}
	}
	clock.now, clock.known = seconds, true

	// Start the timers made before the clock was known, then take the due ones.
	// Timers a callback makes wait for the next frame.
	var due []*Timer
	for _, t := range clock.timers {
		if !t.started {
			t.started = true
			t.at = seconds + t.delay
		}
		if !t.finished && t.at <= seconds {
			due = append(due, t)
		}
	}
	slices.SortStableFunc(due, func(a, b *Timer) int { return cmp.Compare(a.at, b.at) })

	for _, t := range due {
		if !t.finished {
			runTimer(t, seconds)
		}
	}
	clock.timers = slices.DeleteFunc(clock.timers, func(t *Timer) bool { return t.finished })
}

// runTimer runs one due timer and schedules its next run.
func runTimer(t *Timer, seconds float64) {
	defer func() {
		if r := recover(); r != nil {
			t.finished = true
			Log(fmt.Sprintf("timer failed: %v", r))
		}
	}()
	wait, again := t.run()
	if !again {
		t.finished = true
		return
	}
	next := t.at + wait.Seconds()
	if next <= seconds {
		next = seconds + wait.Seconds()
	}
	t.at = next
}
