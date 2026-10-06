package main

import (
	"math"
	"strconv"
)

// clockText writes seconds as minutes and seconds, such as 1:04.2, with
// places digits after the point.
func clockText(seconds float64, places int) string {
	// Split off whole minutes, rounding down so 59.96 reads 0:59.9, not 0:60.0.
	scale := math.Pow(10, float64(places))
	seconds = math.Floor(max(seconds, 0)*scale) / scale
	minutes := int(seconds / 60)
	seconds -= float64(minutes * 60)

	// Pad the seconds to two digits.
	text := strconv.FormatFloat(seconds, 'f', places, 64)
	if seconds < 10 {
		text = "0" + text
	}
	return strconv.Itoa(minutes) + ":" + text
}

// ordinal writes a place as 1st, 2nd, 3rd, 4th and so on.
func ordinal(place int) string {
	suffix := "th"
	switch {
	case place%100 >= 11 && place%100 <= 13:
	case place%10 == 1:
		suffix = "st"
	case place%10 == 2:
		suffix = "nd"
	case place%10 == 3:
		suffix = "rd"
	}
	return strconv.Itoa(place) + suffix
}

// plural writes count with noun, adding an s unless count is one.
func plural(count int, noun string) string {
	if count == 1 {
		return "1 " + noun
	}
	return strconv.Itoa(count) + " " + noun + "s"
}
