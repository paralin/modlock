package main

import (
	"cmp"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/sourcemap"
	"github.com/paralin/modlock/src/host_app/control"
)

// profileService is the service each mod library serves for the profiler.
// Its "read" method turns the library's handler timing on and returns the
// timings since the last read; a mod that never hears from it, as on a
// production server, never times its handlers.
const profileService = "modlock.profile"

// profileCallPrefix starts the id of each of the profiler's calls, so their
// results stay out of the console's output.
const profileCallPrefix = "modlock.profile/"

// profilePoll is how often the profiler reads each mod's timings, and
// profileReport how often it reports them.
const (
	profilePoll   = time.Second
	profileReport = 10 * time.Second
)

// profileShowMs is the mean frame time past which the table prints without
// --perf.
const profileShowMs = 1.0

// profileRows is the most handlers one table lists.
const profileRows = 10

// timingRead is one read of a mod library's timings, as the library writes it.
type timingRead struct {
	// Frames is the whole frames' timings.
	Frames timingRow `json:"frames"`
	// Sites holds each handler that ran.
	Sites []timingRow `json:"sites"`
}

// timingRow is one handler's timings in a read. Times are milliseconds.
type timingRow struct {
	// Kind names the registration, such as onFrame.
	Kind string `json:"kind"`
	// At is the location that registered the handler, as the library found
	// it: a Go file and line, or a bundle's line and column.
	At string `json:"at"`
	// Calls counts the handler's runs.
	Calls uint64 `json:"calls"`
	// Ms is their total time.
	Ms float64 `json:"ms"`
	// Max is the slowest run.
	Max float64 `json:"max"`
	// Over counts the runs longer than 4 ms.
	Over uint64 `json:"over"`
}

// add counts another read's timings into r.
func (r *timingRow) add(o timingRow) {
	r.Calls += o.Calls
	r.Ms += o.Ms
	r.Max = max(r.Max, o.Max)
	r.Over += o.Over
}

// modProfile is what the profiler holds of one mod.
type modProfile struct {
	// dir is the mod's build directory, for its source map, or empty.
	dir string
	// silent is true once the mod answered that it serves no profiler, as a
	// library from before it does.
	silent bool
	// frames and sites total the reads since the last report.
	frames timingRow
	sites  map[string]*timingRow
	order  []string
}

// profiler reads the running mods' handler timings during a dev or play
// session and reports them every 10 seconds: as a table for a person when a
// mod used more than 1 ms a frame, or always with --perf, and as a
// ModTimings event for a program.
type profiler struct {
	out *printer
	// always prints the table whatever the mods' time.
	always bool
	// dirs holds each mod's build directory by slug.
	dirs map[string]string

	mu   sync.Mutex
	mods map[string]*modProfile
	// order holds the mods in the order they started.
	order []string
	// calls counts the reads sent, for their ids.
	calls uint64
}

// newProfiler returns a profiler for the mods built in mods.
func newProfiler(out *printer, mods []string, always bool) *profiler {
	dirs := make(map[string]string, len(mods))
	for _, dir := range mods {
		if manifest, err := project.ReadManifest(dir); err == nil {
			dirs[manifest.GetSlug()] = dir
		}
	}
	return &profiler{out: out, always: always, dirs: dirs, mods: map[string]*modProfile{}}
}

// follow returns a handler that passes each host event to events, keeping
// the profiler's own results back and noting each mod that starts.
func (p *profiler) follow(events func(*control.HostEvent)) func(*control.HostEvent) {
	return func(event *control.HostEvent) {
		if started := event.GetStarted(); started != nil {
			p.started(started.GetMod())
		}
		if called := event.GetCalled(); called != nil && strings.HasPrefix(called.GetId(), profileCallPrefix) {
			p.answered(called)
			return
		}
		events(event)
	}
}

// started begins a mod's timings afresh, as at a reload, so a fix shows at
// once.
func (p *profiler) started(mod string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if _, ok := p.mods[mod]; !ok {
		p.order = append(p.order, mod)
	}
	p.mods[mod] = &modProfile{dir: p.dirs[mod], sites: map[string]*timingRow{}}
}

// answered counts one read's timings into its mod.
func (p *profiler) answered(result *control.ModCallResult) {
	// Find the mod; one that serves no profiler is not asked again.
	p.mu.Lock()
	defer p.mu.Unlock()
	held := p.mods[result.GetMod()]
	if held == nil {
		return
	}
	if failure := result.GetError(); failure != "" {
		held.silent = held.silent || strings.Contains(failure, "serves no service")
		return
	}

	// Count the read's frames and each handler's row into the period.
	var read timingRead
	if err := json.Unmarshal(result.GetPayload(), &read); err != nil {
		return
	}
	held.frames.add(read.Frames)
	for _, row := range read.Sites {
		key := row.Kind + " " + row.At
		total := held.sites[key]
		if total == nil {
			total = &timingRow{Kind: row.Kind, At: row.At}
			held.sites[key] = total
			held.order = append(held.order, key)
		}
		total.add(row)
	}
}

// run reads the mods' timings each second and reports them every 10 seconds
// until ctx ends.
func (p *profiler) run(ctx context.Context, running server) {
	// Read each second and report every period until the session ends.
	poll := time.NewTicker(profilePoll)
	defer poll.Stop()
	report := time.NewTicker(profileReport)
	defer report.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-poll.C:
			p.poll(ctx, running)
		case <-report.C:
			for _, timings := range p.take(profileReport) {
				p.report(timings)
			}
		}
	}
}

// poll asks each running mod that serves the profiler for its timings.
func (p *profiler) poll(ctx context.Context, running server) {
	// Name a read for each mod that may serve the profiler, then send them
	// outside the lock.
	p.mu.Lock()
	var calls []*control.ModCall
	for _, mod := range p.order {
		if p.mods[mod].silent {
			continue
		}
		p.calls++
		calls = append(calls, &control.ModCall{
			Mod:     mod,
			Service: profileService,
			Method:  "read",
			Id:      profileCallPrefix + strconv.FormatUint(p.calls, 10),
		})
	}
	p.mu.Unlock()
	for _, call := range calls {
		_ = running.Call(ctx, call)
	}
}

// take returns each mod's timings since the last report, over a period of
// length, and starts the next period.
func (p *profiler) take(length time.Duration) []*modcli.ModTimings {
	// Take each mod that ran a frame, its slowest handlers first, and start
	// its next period.
	p.mu.Lock()
	defer p.mu.Unlock()
	var taken []*modcli.ModTimings
	for _, mod := range p.order {
		held := p.mods[mod]
		if held.silent || held.frames.Calls == 0 {
			continue
		}
		timings := &modcli.ModTimings{
			Mod:     mod,
			Seconds: uint32(length / time.Second),
			Frames:  handlerTiming(held.dir, &held.frames),
		}
		for _, key := range held.order {
			timings.Handlers = append(timings.Handlers, handlerTiming(held.dir, held.sites[key]))
		}
		slices.SortStableFunc(timings.Handlers, func(a, b *modcli.HandlerTiming) int {
			return cmp.Compare(b.GetTotalMs(), a.GetTotalMs())
		})
		taken = append(taken, timings)
		p.mods[mod] = &modProfile{dir: held.dir, sites: map[string]*timingRow{}}
	}
	return taken
}

// handlerTiming returns a row's timings, naming the source line of a script
// handler through the source map in the mod's build directory dir.
func handlerTiming(dir string, row *timingRow) *modcli.HandlerTiming {
	file, line := locateSite(dir, row.At)
	return &modcli.HandlerTiming{
		Kind:    row.Kind,
		File:    file,
		Line:    line,
		Calls:   row.Calls,
		TotalMs: row.Ms,
		MaxMs:   row.Max,
		Over:    row.Over,
	}
}

// locateSite splits a library's location, "file:line" or "file:line:column",
// into the file and line, mapping a bundle location through the source map
// in dir. A file under the working directory is named from it.
func locateSite(dir, at string) (string, uint32) {
	// Split off the line and the column.
	parts := strings.Split(at, ":")
	numbers := make([]int, 0, 2)
	for len(parts) > 1 && len(numbers) < 2 {
		n, err := strconv.Atoi(parts[len(parts)-1])
		if err != nil {
			break
		}
		numbers = append([]int{n}, numbers...)
		parts = parts[:len(parts)-1]
	}
	file := strings.Join(parts, ":")
	if len(numbers) == 0 {
		return file, 0
	}
	line := numbers[0]

	// Map a script's bundle location to its source.
	if filepath.Base(file) == project.ScriptEntry && len(numbers) == 2 && dir != "" {
		if data, err := os.ReadFile(filepath.Join(dir, project.SourceMapEntry)); err == nil {
			if sources, err := sourcemap.Parse(data); err == nil {
				if found, ok := sources.Locate(line, numbers[1]); ok {
					file, line = filepath.FromSlash(found.Source), found.Line
					if !filepath.IsAbs(file) {
						file = filepath.Join(dir, file)
					}
				}
			}
		}
	}

	// Name a file under the working directory from it.
	if wd, err := os.Getwd(); err == nil && filepath.IsAbs(file) {
		if rel, err := filepath.Rel(wd, file); err == nil && filepath.IsLocal(rel) {
			file = filepath.ToSlash(rel)
		}
	}
	return file, uint32(line)
}

// report sends one mod's timings to a program, and prints them for a person
// when the mod used more than 1 ms a frame or --perf asks.
func (p *profiler) report(timings *modcli.ModTimings) {
	// A program gets every period.
	if p.out.json {
		p.out.emit(&modcli.Event{Body: &modcli.Event_Timings{Timings: timings}})
		return
	}

	// A person gets the table of a mod over 1 ms a frame, or of any with
	// --perf.
	frames := timings.GetFrames()
	mean := frames.GetTotalMs() / float64(max(frames.GetCalls(), 1))
	if !p.always && mean <= profileShowMs {
		return
	}
	for _, line := range timingTable(timings) {
		p.out.line(p.out.pad() + line)
	}
}

// timingTable lays out one mod's timings as the lines of a table: the mod's
// frames, then its slowest handlers.
func timingTable(timings *modcli.ModTimings) []string {
	// The mod's frames head the table.
	frames := timings.GetFrames()
	lines := []string{fmt.Sprintf("%s  %d s, %d frames: mean %.2f ms, max %.2f ms, %d over 4 ms",
		timings.GetMod(), timings.GetSeconds(), frames.GetCalls(),
		frames.GetTotalMs()/float64(max(frames.GetCalls(), 1)), frames.GetMaxMs(), frames.GetOver())}

	// Each handler follows, the most time first, up to profileRows, in a
	// column as wide as the longest name.
	shown := timings.GetHandlers()[:min(len(timings.GetHandlers()), profileRows)]
	width := 44
	for _, handler := range shown {
		width = max(width, len(handlerName(handler)))
	}
	lines = append(lines, fmt.Sprintf("  %-*s %7s %8s %7s %9s", width, "handler", "calls", "mean ms", "max ms", "over 4 ms"))
	for _, handler := range shown {
		lines = append(lines, fmt.Sprintf("  %-*s %7d %8.2f %7.2f %9d",
			width, handlerName(handler), handler.GetCalls(),
			handler.GetTotalMs()/float64(max(handler.GetCalls(), 1)), handler.GetMaxMs(), handler.GetOver()))
	}
	if more := len(timings.GetHandlers()) - len(shown); more > 0 {
		lines = append(lines, fmt.Sprintf("  and %d more", more))
	}
	return lines
}

// handlerName names a handler by its kind and source line, such as
// "onFrame src/bots.ts:40".
func handlerName(handler *modcli.HandlerTiming) string {
	name := handler.GetKind()
	if file := handler.GetFile(); file != "" {
		name += " " + file
		if line := handler.GetLine(); line != 0 {
			name += ":" + strconv.Itoa(int(line))
		}
	}
	return name
}
