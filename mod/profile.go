package mod

import (
	"encoding/json"
	"errors"
	"path/filepath"
	"runtime"
	"strconv"
	"time"
)

// The library times the mod's handlers for the profiler of modlock dev and
// modlock play, which calls the "modlock.profile" service the library serves.
// The first call turns timing on; until then, and on a production server that
// never calls, each dispatch costs one test of profiling and no clock read.
// Each handler is known by its kind, such as OnFrame, and the file and line
// that registered it.

// profileService is the service the profiler calls.
const profileService = "modlock.profile"

// overBudget is the time past which one call or frame counts as slow.
const overBudget = 4 * time.Millisecond

// profiling is true while the profiler reads the timings.
var profiling bool

// site is one registered handler and its timings since the last read.
type site struct {
	// kind names the registration, such as OnFrame, and at is the file and
	// line that registered it.
	kind, at string
	calls    int
	total    time.Duration
	max      time.Duration
	// over counts the calls longer than overBudget.
	over int
}

// add counts one call that took spent.
func (s *site) add(spent time.Duration) {
	s.calls++
	s.total += spent
	s.max = max(s.max, spent)
	if spent > overBudget {
		s.over++
	}
}

// profiled holds every site in registration order, sitesByKey finds each by
// its kind and location, and frameTimes counts whole frames.
var (
	profiled   []*site
	sitesByKey = map[string]*site{}
	frameTimes = &site{kind: "frame"}
)

// sitesOf holds each handler list's sites, by the list's kind, in the order
// of its handlers.
var sitesOf = map[string][]*site{}

// track records a handler of kind that the mod registers. Call it directly
// from the function the mod called, so its caller is the mod's line.
func track(kind string) *site {
	tracked := siteAt(kind, caller(3))
	sitesOf[kind] = append(sitesOf[kind], tracked)
	return tracked
}

// caller returns the file and line skip frames up, counting caller's own
// caller as 1, or empty.
func caller(skip int) string {
	if _, file, line, ok := runtime.Caller(skip); ok {
		return file + ":" + strconv.Itoa(line)
	}
	return ""
}

// siteAt returns the site of kind at a location, made on first use.
func siteAt(kind, at string) *site {
	// Reuse the site made for this kind and location, or make it.
	key := kind + " " + at
	if held, ok := sitesByKey[key]; ok {
		return held
	}
	made := &site{kind: kind, at: at}
	sitesByKey[key] = made
	profiled = append(profiled, made)
	return made
}

// timed calls fn and counts its time to at.
func timed(at *site, fn func()) {
	start := now()
	defer func() { at.add(now().Sub(start)) }()
	fn()
}

// now reads the clock that times handlers; a test counts its reads.
var now = time.Now

// eachTimed calls every handler with event, timing each while profiling;
// kind names the list's sites.
func eachTimed[E any](handlers []func(E), kind string, event E) {
	if !profiling {
		for _, handler := range handlers {
			handler(event)
		}
		return
	}
	eachWith(handlers, sitesOf[kind], func(handler func(E)) { handler(event) })
}

// eachWith calls call with every handler, timing each against its site in
// at. Call it only while profiling.
func eachWith[H any](handlers []H, at []*site, call func(H)) {
	for i, handler := range handlers {
		if i < len(at) {
			timed(at[i], func() { call(handler) })
		} else {
			call(handler)
		}
	}
}

// timingRow is one site's timings in the profiler's answer. Times are
// milliseconds.
type timingRow struct {
	// Kind names the registration, such as OnFrame.
	Kind string `json:"kind"`
	// At is the file and line that registered the handler.
	At string `json:"at"`
	// Calls counts the handler's runs.
	Calls int `json:"calls"`
	// Ms is their total time.
	Ms float64 `json:"ms"`
	// Max is the slowest run.
	Max float64 `json:"max"`
	// Over counts the runs longer than overBudget.
	Over int `json:"over"`
}

// timings is the profiler's answer.
type timings struct {
	// Frames is the whole frames' timings.
	Frames timingRow `json:"frames"`
	// Sites holds each handler that ran.
	Sites []timingRow `json:"sites"`
}

// rowOf returns a site's timings and resets them.
func rowOf(s *site) timingRow {
	row := timingRow{Kind: s.kind, At: filepath.ToSlash(s.at), Calls: s.calls, Ms: millis(s.total), Max: millis(s.max), Over: s.over}
	s.calls, s.total, s.max, s.over = 0, 0, 0, 0
	return row
}

// millis returns d in milliseconds.
func millis(d time.Duration) float64 {
	return float64(d) / float64(time.Millisecond)
}

// readProfile answers the profiler: method "read" turns timing on and returns
// the timings since the last read as JSON, resetting them, and "stop" turns
// it off.
func readProfile(method string) ([]byte, error) {
	// Stop turns timing off; any method but read is refused.
	switch method {
	case "stop":
		profiling = false
		return nil, nil
	case "read":
	default:
		return nil, errors.New(profileService + " has no method " + method)
	}

	// Report the sites that ran while timing was on, resetting every count,
	// and turn timing on.
	counted := profiling
	report := timings{Sites: []timingRow{}}
	report.Frames = rowOf(frameTimes)
	for _, s := range profiled {
		row := rowOf(s)
		if counted && row.Calls != 0 {
			report.Sites = append(report.Sites, row)
		}
	}
	if !counted {
		report.Frames = timingRow{Kind: "frame"}
	}
	profiling = true
	return json.Marshal(report)
}
