package main

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/studio"
	"github.com/pkg/errors"
)

// studioRenderCommand draws models from the game's archive to PNG files
// with a transparent ground, through Studio's page in a headless browser.
func studioRenderCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "render",
		Usage:     "render models from the game's archive to transparent PNG files",
		ArgsUsage: "[NAME=]MODEL...",
		Description: "Each MODEL is an archive path or a pattern, such as models/props_street/*.vmdl_c, where\n" +
			"* stops at a slash. Every model is drawn alone at each view, lit by a rig that turns with the\n" +
			"camera, trimmed to the model with an even margin, and saved as NAME-VIEW.png, where NAME is\n" +
			"the model's file name unless given. A view is NAME:YAW:PITCH in degrees; yaw 0 looks at the\n" +
			"model's front and positive yaw circles counterclockwise seen from above, toward the model's left.\n\n" +
			"The page draws in a headless Chromium found on the PATH or given by --browser; with --browser\n" +
			"none, open the printed address in any browser with WebGL 2.",
		Flags: []cli.Flag{
			&cli.StringFlag{Name: "out", Value: ".", Usage: "the `DIR` to write the pictures to"},
			&cli.StringFlag{Name: "list", Usage: "read more models from `FILE`, one [NAME=]MODEL per line; # starts a comment"},
			&cli.StringSliceFlag{Name: "view", Usage: "draw each model from `NAME:YAW:PITCH`; repeat for several (default left:35:15 and right:-35:15)"},
			&cli.IntFlag{Name: "turntable", Usage: "also draw `N` views evenly around each model, named yaw000 and on"},
			&cli.Float64Flag{Name: "turntable-pitch", Value: 15, Usage: "the turntable views' `PITCH` in degrees"},
			&cli.Float64Flag{Name: "fov", Value: 30, Usage: "the camera's vertical field of view in `DEGREES`"},
			&cli.IntFlag{Name: "size", Value: 1024, Usage: "each picture's longer side in `PIXELS`"},
			&cli.Float64Flag{Name: "margin", Value: 0.04, Usage: "the empty border as a `FRACTION` of the longer side"},
			&cli.StringFlag{Name: "tint", Value: "ffffff", Usage: "the model color as six hexadecimal RGB digits"},
			&cli.StringFlag{Name: "browser", Usage: "the Chromium `PATH` to draw in, or none to print the address instead"},
			&cli.IntFlag{Name: "port", Usage: "the loopback `PORT` to serve on, or a free one when taken or zero"},
		},
		Action: func(c *cli.Context) error {
			out.header("studio render", "")

			// Read the model tint before opening the archive or starting a browser.
			tintText := strings.TrimPrefix(c.String("tint"), "#")
			tintColor, tintErr := strconv.ParseUint(tintText, 16, 24)
			if tintErr != nil || len(tintText) != 6 {
				return errors.New("tint must be six hexadecimal RGB digits")
			}
			tint := []float32{float32((tintColor>>16)&255) / 255, float32((tintColor>>8)&255) / 255, float32(tintColor&255) / 255}

			// Read the views and the models' names and patterns.
			views, err := renderViews(c.StringSlice("view"), c.Int("turntable"), c.Float64("turntable-pitch"))
			if err != nil {
				return err
			}
			targets := c.Args().Slice()
			if file := c.String("list"); file != "" {
				listed, err := readRenderList(file)
				if err != nil {
					return err
				}
				targets = append(targets, listed...)
			}
			if len(targets) == 0 {
				return errors.New("name at least one model")
			}

			// Open the archive and find every model.
			page, assets, err := openStudio(c.Context, out)
			if err != nil {
				return err
			}
			if err := assets.Err(); err != nil {
				return err
			}
			models, err := matchModels(assets, targets)
			if err != nil {
				return err
			}

			// List a shot for each model at each view.
			type output struct{ file, model string }
			var shots []*studio.Shot
			var outputs []output
			dir := c.String("out")
			for _, model := range models {
				for _, view := range views {
					shots = append(shots, &studio.Shot{
						Model:  model.path,
						Tint:   tint,
						Yaw:    view.yaw,
						Pitch:  view.pitch,
						Fov:    float32(c.Float64("fov")),
						Size:   uint32(c.Int("size")),
						Margin: float32(c.Float64("margin")),
					})
					outputs = append(outputs, output{filepath.Join(dir, model.name+"-"+view.name+".png"), model.path})
				}
			}
			if err := os.MkdirAll(dir, 0o755); err != nil {
				return err
			}

			// Save each picture as the page sends it, counting failures.
			failed := 0
			render := studio.NewRender(shots, func(index int, result *studio.ShotResult) error {
				o := outputs[index]
				for _, missing := range result.GetMissing() {
					out.mark("!", yellow, o.model+": drawn without "+missing)
				}
				if result.GetError() != "" {
					failed++
					out.mark("✗", red, o.model+": "+result.GetError())
					return nil
				}
				if err := os.WriteFile(o.file, result.GetPng(), 0o644); err != nil {
					return err
				}
				out.mark("✓", green, o.file)
				return nil
			})

			// Serve the page and draw the job in the browser.
			listener, err := listenLoopback(c.Int("port"))
			if err != nil {
				return err
			}
			address := "http://" + listener.Addr().String() + studio.RenderPage
			ctx, cancel := context.WithCancel(c.Context)
			defer cancel()
			served := make(chan error, 1)
			go func() { served <- serveStudio(ctx, listener, studio.Handler(page, assets, render)) }()
			browserDone := make(chan error, 1)
			browsing := false
			if browser := c.String("browser"); browser == "none" {
				out.field("open", address)
			} else {
				exe, err := findChromium(browser)
				if err != nil {
					return err
				}
				browsing = true
				go func() { browserDone <- runHeadless(ctx, exe, address) }()
			}

			// On every return, stop the browser and wait until it has exited
			// and its profile is removed, so none outlives the command.
			defer func() {
				cancel()
				if browsing {
					<-browserDone
				}
			}()

			// Wait for every shot, the browser leaving early or an interrupt.
			select {
			case <-render.Done():
			case err := <-browserDone:
				browsing = false
				return errors.Wrap(orExited(err), "the browser left before every shot was drawn")
			case err := <-served:
				return err
			case <-c.Context.Done():
				return c.Context.Err()
			}
			if err := render.Err(); err != nil {
				return err
			}
			out.field("rendered", fmt.Sprintf("%d of %d shots", len(shots)-failed, len(shots)))
			if failed != 0 {
				return errors.Errorf("%d shots failed", failed)
			}
			return nil
		},
	}
}

// renderView is one camera position around a model.
type renderView struct {
	name       string
	yaw, pitch float32
}

// renderViews reads the --view flags, or the default pair, then adds n
// turntable views at pitch.
func renderViews(flags []string, n int, pitch float64) ([]renderView, error) {
	if len(flags) == 0 && n == 0 {
		flags = []string{"left:35:15", "right:-35:15"}
	}
	var views []renderView
	for _, flag := range flags {
		parts := strings.Split(flag, ":")
		if len(parts) != 3 || parts[0] == "" {
			return nil, errors.Errorf("view %q is not NAME:YAW:PITCH", flag)
		}
		yaw, err := strconv.ParseFloat(parts[1], 32)
		if err != nil {
			return nil, errors.Wrapf(err, "view %q", flag)
		}
		p, err := strconv.ParseFloat(parts[2], 32)
		if err != nil {
			return nil, errors.Wrapf(err, "view %q", flag)
		}
		views = append(views, renderView{parts[0], float32(yaw), float32(p)})
	}
	for i := range n {
		yaw := float32(360 * i / n)
		views = append(views, renderView{fmt.Sprintf("yaw%03.0f", yaw), yaw, float32(pitch)})
	}
	return views, nil
}

// readRenderList reads the models a list file names, one per line, skipping
// blank lines and comments.
func readRenderList(file string) ([]string, error) {
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var targets []string
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line, _, _ := strings.Cut(scanner.Text(), "#")
		if line = strings.TrimSpace(line); line != "" {
			targets = append(targets, line)
		}
	}
	return targets, scanner.Err()
}

// renderModel is a model to draw and the name its pictures take.
type renderModel struct{ name, path string }

// matchModels finds the models each [NAME=]MODEL target names. A named
// target must match one model; a pattern's models take their file names.
func matchModels(assets *studio.Assets, targets []string) ([]renderModel, error) {
	var models []renderModel
	for _, target := range targets {
		name, pattern, named := strings.Cut(target, "=")
		if !named {
			pattern = target
		}
		found, err := assets.Match(pattern)
		if err != nil {
			return nil, errors.Wrapf(err, "model %q", target)
		}
		if len(found) == 0 {
			return nil, errors.Errorf("no model in the archive matches %q", pattern)
		}
		if named && len(found) > 1 {
			return nil, errors.Errorf("%q matches %d models; a named model must match one", target, len(found))
		}
		for _, p := range found {
			if !named {
				name = strings.TrimSuffix(path.Base(p), ".vmdl_c")
			}
			models = append(models, renderModel{name, p})
		}
	}
	return models, nil
}

// findChromium returns the Chromium to draw in: path when given, else the
// first Chromium or Chrome on the PATH or in its usual install place.
func findChromium(given string) (string, error) {
	if given != "" {
		return given, nil
	}
	for _, name := range []string{"chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "chrome"} {
		if exe, err := exec.LookPath(name); err == nil {
			return exe, nil
		}
	}
	var places []string
	switch runtime.GOOS {
	case "darwin":
		places = []string{
			"/Applications/Chromium.app/Contents/MacOS/Chromium",
			"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
		}
	case "windows":
		for _, root := range []string{os.Getenv("ProgramFiles"), os.Getenv("ProgramFiles(x86)"), os.Getenv("LocalAppData")} {
			if root != "" {
				places = append(places, filepath.Join(root, "Google", "Chrome", "Application", "chrome.exe"))
			}
		}
	}
	for _, place := range places {
		if _, err := os.Stat(place); err == nil {
			return place, nil
		}
	}
	return "", errors.New("no Chromium found; pass its path with --browser, or --browser none to open the page yourself")
}

// runHeadless opens address in a headless Chromium with a fresh profile,
// drawing WebGL in software when the PC has no GPU it can use, until ctx
// ends or the browser exits.
func runHeadless(ctx context.Context, exe, address string) error {
	profile, err := os.MkdirTemp("", "modlock-render-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(profile)
	cmd := exec.CommandContext(ctx, exe,
		"--headless=new",
		"--user-data-dir="+profile,
		"--no-first-run",
		"--enable-unsafe-swiftshader",
		"--ignore-gpu-blocklist",
		"--window-size=640,480",
		address,
	)
	output, err := cmd.CombinedOutput()
	if ctx.Err() != nil {
		return nil
	}
	if err != nil {
		return errors.Wrap(err, lastLine(string(output)))
	}
	return nil
}

// orExited is err, or an error saying the browser exited when it gave none.
func orExited(err error) error {
	if err == nil {
		return errors.New("browser exited")
	}
	return err
}

// lastLine returns the last nonblank line of text.
func lastLine(text string) string {
	lines := strings.Split(strings.TrimSpace(text), "\n")
	return lines[len(lines)-1]
}
