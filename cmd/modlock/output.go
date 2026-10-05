package main

import (
	"context"
	"fmt"
	"os"
	"sync"

	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/proto/modlock/cli"
	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/paralin/modlock/proto/modlock/publish"
	"github.com/pkg/errors"
)

// printer reports progress as text for a person or, with --json, as one
// cli.Event per line on standard output for a program.
type printer struct {
	// json selects the event lines.
	json bool
	// mu keeps lines from the build and the server whole.
	mu sync.Mutex
}

// note prints one line of progress.
func (p *printer) note(parts ...any) {
	// Send the line as an event to a program.
	text := fmt.Sprintln(parts...)
	if p.json {
		p.emit(&cli.Event{Body: &cli.Event_Note{Note: text[:len(text)-1]}})
		return
	}

	// Print the line for a person.
	p.mu.Lock()
	defer p.mu.Unlock()
	_, _ = os.Stdout.WriteString(text)
}

// building reports that a build of the project in dir started.
func (p *printer) building(dir string) {
	if p.json {
		p.emit(&cli.Event{Body: &cli.Event_Building{Building: &cli.Building{Dir: dir}}})
	}
}

// built reports the build of the project in dir that ended with err.
func (p *printer) built(dir string, err error) {
	// A person reads the tools' own output instead.
	if !p.json {
		return
	}

	// Describe the result and locate each problem.
	built := &cli.Built{Dir: dir, Ok: err == nil}
	if err != nil {
		built.Error = err.Error()
	}
	var failed *project.CheckError
	if errors.As(err, &failed) {
		built.Diagnostics = failed.Diagnostics
	}
	p.emit(&cli.Event{Body: &cli.Event_Built{Built: built}})
}

// published reports a release the provider recorded at location.
func (p *printer) published(provider string, release *publish.Release, location string) {
	if p.json {
		p.emit(&cli.Event{Body: &cli.Event_Published{Published: &cli.Published{
			Provider: provider,
			Release:  release,
			Location: location,
		}}})
	}
}

// host reports one event from the server.
func (p *printer) host(event *control.HostEvent) {
	// Send every event, interface changes included, to a program.
	if p.json {
		p.emit(&cli.Event{Body: &cli.Event_Host{Host: event}})
		return
	}

	// Describe the events a person follows.
	switch {
	case event.GetReady() != nil:
		p.note("server ready on", event.GetReady().GetMap())
	case event.GetJoined() != nil:
		p.note(event.GetJoined().GetName(), "joined")
	case event.GetStarted() != nil && event.GetStarted().GetReloaded():
		p.note(event.GetStarted().GetMod(), "reloaded")
	case event.GetStarted() != nil:
		p.note(event.GetStarted().GetMod(), "started")
	case event.GetLog() != nil:
		p.note("[" + event.GetLog().GetMod() + "] " + event.GetLog().GetText())
	case event.GetFailed() != nil && event.GetFailed().GetMod() == "":
		fmt.Fprintln(os.Stderr, event.GetFailed().GetError())
	case event.GetFailed() != nil:
		fmt.Fprintf(os.Stderr, "%s: %s\n", event.GetFailed().GetMod(), event.GetFailed().GetError())
	}
}

// emit writes event as one JSON line.
func (p *printer) emit(event *cli.Event) {
	// Encode the event.
	data, err := event.MarshalJSON()
	if err != nil {
		return
	}

	// Write the line whole.
	p.mu.Lock()
	defer p.mu.Unlock()
	_, _ = os.Stdout.Write(append(data, '\n'))
}

// build builds the project, reporting the build's start and result. Tool
// output goes to standard error.
func (p *printer) build(ctx context.Context, opened *project.Project) error {
	p.building(opened.Dir)
	err := opened.Build(ctx, os.Stderr)
	p.built(opened.Dir, err)
	return err
}
