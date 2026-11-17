package main

import (
	"encoding/json"
	"os"
	"time"

	"github.com/aperturerobotics/cli"
	modcli "github.com/paralin/modlock/cli"
	"github.com/pkg/errors"
)

// profileCommand runs a mod in the sandbox, timing its handlers, and writes
// the timings as a flame graph speedscope opens.
func profileCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "profile",
		Usage:     "time the mod's handlers in the sandbox and write a flame graph",
		ArgsUsage: "[DIRECTORY]",
		Description: "profile builds the mod and runs it alone in the sandbox for --seconds, timing each\n" +
			"handler by the line that registered it. It prints the table modlock dev prints and\n" +
			"writes the timings to --out in speedscope's format: open the file at\n" +
			"https://www.speedscope.app, where each handler's width is the time it spent.",
		Flags: []cli.Flag{
			releaseFlag,
			&cli.IntFlag{Name: "seconds", Value: 10, Usage: "how long to run the mod"},
			&cli.StringFlag{Name: "out", Value: "profile.speedscope.json", Usage: "the file to write the flame graph to"},
		},
		Action: func(c *cli.Context) error {
			// Build the mod.
			seconds := c.Int("seconds")
			if seconds < 1 {
				return errors.New("--seconds must be at least 1")
			}
			opened, err := openProject(c)
			if err != nil {
				return err
			}
			if err := out.build(c.Context, opened, buildMode(c)); err != nil {
				return err
			}

			// Time it, printing the table.
			out.note("profiling", opened.Manifest.GetSlug(), "for", seconds, "s in the sandbox")
			prof := newProfiler(out, []string{opened.Output()}, true)
			taken := timeInSandbox(c.Context, prof, opened.Output(), hostInterpreters, time.Duration(seconds)*time.Second)
			if err := c.Context.Err(); err != nil {
				return err
			}
			if len(taken) == 0 {
				return errors.New("the mod ran no frames with the profiler; rebuild it with this modlock, whose library times its handlers")
			}
			for _, timings := range taken {
				prof.report(timings)
			}

			// Write the flame graph.
			data, err := speedscopeFile(taken)
			if err != nil {
				return err
			}
			if err := os.WriteFile(c.String("out"), data, 0o644); err != nil {
				return errors.Wrap(err, "write the flame graph")
			}
			out.note("wrote", c.String("out"))
			return nil
		},
	}
}

// speedscopeSchema names the file format speedscope reads.
const speedscopeSchema = "https://www.speedscope.app/file-format-schema.json"

// speedscopeFrame is one function of a speedscope file.
type speedscopeFrame struct {
	// Name names the function, as the timing table does.
	Name string `json:"name"`
	// File is the function's source file, when known.
	File string `json:"file,omitempty"`
	// Line is its line in File, when known.
	Line uint32 `json:"line,omitempty"`
}

// speedscopeProfile is one sampled profile of a speedscope file: each
// sample is a stack of frame indexes, root first, with its weight.
type speedscopeProfile struct {
	// Type is "sampled".
	Type string `json:"type"`
	// Name names the profile, after its mod.
	Name string `json:"name"`
	// Unit is the unit of the weights and values.
	Unit string `json:"unit"`
	// StartValue is where the profile starts, 0.
	StartValue float64 `json:"startValue"`
	// EndValue is where it ends, the total of the weights.
	EndValue float64 `json:"endValue"`
	// Samples holds each stack.
	Samples [][]int `json:"samples"`
	// Weights holds each stack's time.
	Weights []float64 `json:"weights"`
}

// speedscopeFile returns the timings as a speedscope file with a profile
// for each mod: the mod at the root and each handler above it, its width the
// milliseconds it spent.
func speedscopeFile(taken []*modcli.ModTimings) ([]byte, error) {
	// Share one frame for each function among the profiles.
	var frames []speedscopeFrame
	index := map[speedscopeFrame]int{}
	frameOf := func(frame speedscopeFrame) int {
		if at, ok := index[frame]; ok {
			return at
		}
		index[frame] = len(frames)
		frames = append(frames, frame)
		return len(frames) - 1
	}

	// Stack each of a mod's handlers on the mod.
	profiles := make([]speedscopeProfile, 0, len(taken))
	for _, timings := range taken {
		mod := frameOf(speedscopeFrame{Name: timings.GetMod()})
		profile := speedscopeProfile{Type: "sampled", Name: timings.GetMod(), Unit: "milliseconds"}
		for _, handler := range timings.GetHandlers() {
			frame := frameOf(speedscopeFrame{Name: handlerName(handler), File: handler.GetFile(), Line: handler.GetLine()})
			profile.Samples = append(profile.Samples, []int{mod, frame})
			profile.Weights = append(profile.Weights, handler.GetTotalMs())
			profile.EndValue += handler.GetTotalMs()
		}
		profiles = append(profiles, profile)
	}
	return json.MarshalIndent(map[string]any{
		"$schema":  speedscopeSchema,
		"name":     "modlock profile",
		"exporter": "modlock " + Version,
		"shared":   map[string]any{"frames": frames},
		"profiles": profiles,
	}, "", "  ")
}
