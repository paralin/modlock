package main

import (
	"path/filepath"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/session"
	"github.com/pkg/errors"
)

// devCommand runs a mod in a local server and reloads it on each save.
func devCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:      "dev",
		Usage:     "run the mod in a local server, join it, and reload the mod on each save",
		ArgsUsage: "[DIRECTORY]",
		Flags:     sessionFlags,
		Action: func(c *cli.Context) error {
			// Open the project.
			opened, err := openProject(c)
			if err != nil {
				return err
			}

			// Build now and after each save. The first good build starts the
			// server and each later one reloads the mod; a failed build keeps
			// the running one. Watch calls rebuild on one goroutine.
			var running *session.Session
			started := make(chan error, 1)
			rebuild := func() {
				// Keep the running mod when the build fails.
				if err := out.build(c.Context, opened); err != nil {
					out.note("the build failed; fix it and save to try again")
					return
				}

				// Reload a running server, or start the first one.
				if running != nil {
					_ = running.Reload(c.Context, opened.Output())
					return
				}
				var err error
				running, err = startSession(c, out, []string{opened.Output()}, filepath.Join(opened.Output(), "server.log"))
				started <- err
			}

			// Build once, then after each save.
			go func() {
				rebuild()
				err := opened.Watch(c.Context, func() {
					out.note("rebuilding", opened.Manifest.GetSlug())
					rebuild()
				})
				if err != nil && c.Context.Err() == nil {
					out.note(errors.Wrap(err, "stopped watching").Error())
				}
			}()

			// Run the session once it starts.
			select {
			case <-c.Context.Done():
				return nil
			case err := <-started:
				if err != nil {
					return err
				}
			}
			defer running.Stop()
			return waitSession(c, out, running)
		},
	}
}
