package mod

import (
	"slices"
	"testing"
	"time"
)

// frame advances the game clock to seconds and runs the timers that are due.
func frame(seconds float64) {
	advanceTimers(seconds)
}

// resetClock forgets every timer and the clock, as at the start of a mod.
func resetClock(t *testing.T) {
	t.Helper()
	clock.timers, clock.known, clock.now = nil, false, 0
}

func TestAfterAndEvery(t *testing.T) {
	resetClock(t)
	frame(100)
	var runs []string
	After(2*time.Second, func() { runs = append(runs, "after") })
	Every(time.Second, func() { runs = append(runs, "every") })

	frame(100.5)
	frame(101)
	frame(102.2)
	if want := []string{"every", "after", "every"}; !slices.Equal(runs, want) {
		t.Fatalf("runs = %v, want %v", runs, want)
	}

	// The late frame at 102.2 ran the repeat due at 102, so the next is at 103.
	frame(102.9)
	if len(runs) != 3 {
		t.Fatalf("ran early: %v", runs)
	}
	frame(103.1)
	if len(runs) != 4 {
		t.Fatalf("missed the repeat at 103: %v", runs)
	}

	// A new map's clock starts over, and the repeat keeps its remaining delay.
	frame(0)
	frame(0.8)
	if len(runs) != 4 {
		t.Fatalf("ran early after the clock went back: %v", runs)
	}
	frame(1)
	if len(runs) != 5 {
		t.Fatalf("missed the repeat after the clock went back: %v", runs)
	}
}

func TestCancelAndStartBeforeFirstFrame(t *testing.T) {
	resetClock(t)
	ran := 0
	timer := Every(time.Second, func() { ran++ })
	frame(500)
	frame(500.5)
	if ran != 0 {
		t.Fatalf("a timer made before the first frame ran at once: %d", ran)
	}
	frame(501)
	timer.Cancel()
	frame(510)
	if ran != 1 || !timer.Finished() {
		t.Fatalf("ran = %d, finished = %v", ran, timer.Finished())
	}
}

func TestSequence(t *testing.T) {
	resetClock(t)
	frame(0)
	var runs []int
	timer := Sequence(func(run int) (time.Duration, bool) {
		runs = append(runs, run)
		return 500 * time.Millisecond, run < 3
	})
	frame(0)
	frame(0.4)
	frame(0.5)
	frame(1)
	if want := []int{1, 2, 3}; !slices.Equal(runs, want) || !timer.Finished() {
		t.Fatalf("runs = %v, finished = %v", runs, timer.Finished())
	}
}

func TestPanicEndsOnlyItsTimer(t *testing.T) {
	resetClock(t)
	frame(0)
	survivor := 0
	broken := Every(time.Second, func() { panic("boom") })
	Every(time.Second, func() { survivor++ })
	frame(1)
	frame(2)
	if !broken.Finished() || survivor != 2 {
		t.Fatalf("broken finished = %v, survivor = %d", broken.Finished(), survivor)
	}
}

func TestTimerMadeInCallbackWaitsForNextFrame(t *testing.T) {
	resetClock(t)
	frame(0)
	var runs []string
	After(0, func() {
		runs = append(runs, "outer")
		After(0, func() { runs = append(runs, "inner") })
	})
	frame(0)
	if !slices.Equal(runs, []string{"outer"}) {
		t.Fatalf("runs = %v", runs)
	}
	frame(0)
	if !slices.Equal(runs, []string{"outer", "inner"}) {
		t.Fatalf("runs = %v", runs)
	}
}
