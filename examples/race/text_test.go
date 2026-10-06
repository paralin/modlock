package main

import "testing"

// TestClockText checks the race clock's minutes, padding and rounding.
func TestClockText(t *testing.T) {
	for _, c := range []struct {
		// seconds is the time to write.
		seconds float64
		// places is the digits after the point.
		places int
		// want is the expected text.
		want string
	}{
		{0, 1, "0:00.0"},
		{9.96, 1, "0:09.9"},
		{59.96, 1, "0:59.9"},
		{64.25, 1, "1:04.2"},
		{125.9, 0, "2:05"},
	} {
		if got := clockText(c.seconds, c.places); got != c.want {
			t.Errorf("clockText(%v, %d) = %q, want %q", c.seconds, c.places, got, c.want)
		}
	}
}

// TestOrdinal checks the suffixes, including the teens.
func TestOrdinal(t *testing.T) {
	for place, want := range map[int]string{1: "1st", 2: "2nd", 3: "3rd", 4: "4th", 11: "11th", 12: "12th", 21: "21st", 113: "113th"} {
		if got := ordinal(place); got != want {
			t.Errorf("ordinal(%d) = %q, want %q", place, got, want)
		}
	}
}
