package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"unicode"
	"unicode/utf8"

	"github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/publish"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/pkg/errors"
)

// printer reports progress as text for a person or, with --json, as one
// cli.Event per line on standard output for a program.
type printer struct {
	// json selects the event lines.
	json bool
	// brief keeps a person's output to short lines, for host and join: no
	// header or fields, and progress without its symbol.
	brief bool
	// color and errColor are true when standard output and standard error
	// show color.
	color, errColor bool
	// current is the running command line's release.
	current string
	// watchers also receive every event the printer reports. Add them with
	// tap before the first report.
	watchers []func(*cli.Event)

	// mu keeps lines from the build and the server whole, and guards the
	// fields below.
	mu sync.Mutex
	// session is true for a dev or play command, whose header carries the
	// update notice, and headed once that header printed.
	session, headed bool
	// latest is a newer release an update check found, and noticed is true
	// once the person was told about it.
	latest  string
	noticed bool
	// names holds each joined player's name by slot.
	names map[int32]string
}

// header opens a session's output with the command line's release, the
// command and what it runs, and the update notice once one is known.
func (p *printer) header(command, name string) {
	// A program reads the events instead.
	if p.json {
		return
	}

	// Name the session, unless brief.
	p.mu.Lock()
	p.session, p.headed = true, true
	if p.brief {
		p.mu.Unlock()
		p.notice()
		return
	}
	_, _ = os.Stdout.WriteString("\n  " + paint(p.color, bold, "modlock "+p.current) + paint(p.color, dim, "  "+command+" "+name) + "\n\n")
	p.mu.Unlock()
	p.notice()
}

// field prints one labeled fact a person may need, such as an address.
// Brief output leaves it out.
func (p *printer) field(label, value string) {
	switch {
	case p.json:
		p.note(strings.TrimSpace(label)+":", value)
		return
	case p.brief:
		return
	}
	p.line(fmt.Sprintf("  %s %s", paint(p.color, dim, fmt.Sprintf("%-8s", label)), value))
}

// mark prints one line of a session's progress after a symbol in t. Brief
// output keeps the symbol only for a warning or a failure, and starts the
// line with a capital.
func (p *printer) mark(symbol string, t tone, text string) {
	switch {
	case p.json:
		p.note(text)
	case p.brief && t != yellow && t != red:
		first, size := utf8.DecodeRuneInString(text)
		p.line(string(unicode.ToUpper(first)) + text[size:])
	default:
		p.line(p.pad() + paint(p.color, t, symbol) + " " + text)
	}
}

// pad returns the indent of a session's lines, which brief output leaves out.
func (p *printer) pad() string {
	if p.brief {
		return ""
	}
	return "  "
}

// line prints text as one whole line on standard output.
func (p *printer) line(text string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	_, _ = os.Stdout.WriteString(text + "\n")
}

// update reports latest, a newer release than the running one. A program
// gets an event at once. A person reading a session gets a framed notice in
// its header, or as soon as it is known after that; anyone else gets a line
// when the command ends.
func (p *printer) update(latest string) {
	// Keep the newest release found.
	p.mu.Lock()
	p.latest, p.noticed = latest, false
	headed := p.headed
	p.mu.Unlock()

	// Tell a program, or a session already under way.
	if p.json {
		p.emit(&cli.Event{Body: &cli.Event_Update{Update: &cli.Update{Current: p.current, Latest: latest}}})
		return
	}
	if headed {
		p.notice()
	}
}

// notice tells a person on a terminal about the newer release, once.
func (p *printer) notice() {
	// Tell only a person, about a release not yet noticed.
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.json || p.latest == "" || p.noticed || !terminal(os.Stderr) {
		return
	}
	p.noticed = true

	// Frame it in a session, or keep it to one line after another command.
	paintErr := func(t tone, text string) string { return paint(p.errColor, t, text) }
	if !p.session {
		_, _ = fmt.Fprintf(os.Stderr, "%s modlock %s is out, and this is %s; run %s to install it\n",
			paintErr(yellow, "!"), paintErr(bold, p.latest), p.current, paintErr(cyan, "modlock update"))
		return
	}
	box := frame([]string{
		"modlock " + paintErr(bold+";"+green, p.latest) + " is out, and this is " + paintErr(dim, p.current),
		"run " + paintErr(cyan, "modlock update") + " to install it",
		paintErr(dim, "modlock config set update-check off stops this check"),
	}, func(edge string) string { return paintErr(yellow, edge) })
	_, _ = os.Stderr.WriteString(indent(box) + "\n")
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
	p.line(text[:len(text)-1])
}

// building reports that a build of the project in dir started.
func (p *printer) building(dir string) {
	event := &cli.Event{Body: &cli.Event_Building{Building: &cli.Building{Dir: dir}}}
	p.observe(event)
	if p.json {
		p.emit(event)
	}
}

// built reports the build of the project in dir that ended with err.
func (p *printer) built(dir string, err error) {
	// Describe the result and locate each problem.
	built := &cli.Built{Dir: dir, Ok: err == nil}
	if err != nil {
		built.Error = err.Error()
	}
	var failed *project.CheckError
	if errors.As(err, &failed) {
		built.Diagnostics = failed.Diagnostics
	}
	event := &cli.Event{Body: &cli.Event_Built{Built: built}}
	p.observe(event)

	// A person reads the tools' own output for problems, and a session marks
	// each good build.
	p.mu.Lock()
	session := p.session
	p.mu.Unlock()
	switch {
	case p.json:
		p.emit(event)
	case session && err == nil && !p.brief:
		p.mark("✓", green, "built "+filepath.Base(dir))
	}
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

// sandbox reports that the session runs in the sandbox, and why.
func (p *printer) sandbox(reason string) {
	if p.json {
		p.emit(&cli.Event{Body: &cli.Event_Sandbox{Sandbox: &cli.Sandbox{Reason: reason}}})
		return
	}
	p.mark("●", yellow, reason+"; running the mods in the sandbox, without the game, with a stand-in player")
	p.field("input", "type a command, such as /hello, and press Enter to send it as the player")
}

// host reports one event from the server.
func (p *printer) host(event *control.HostEvent) {
	// Send every event, interface changes included, to a program.
	wrapped := &cli.Event{Body: &cli.Event_Host{Host: event}}
	p.observe(wrapped)
	if p.json {
		p.emit(wrapped)
		return
	}

	// Describe the events a person follows.
	switch name := p.name(event); {
	case event.GetReady() != nil && event.GetReady().GetMap() == "":
		p.mark("●", green, "ready")
	case event.GetReady() != nil:
		p.mark("●", green, "server ready on "+event.GetReady().GetMap())
	case event.GetJoined() != nil:
		p.mark("→", cyan, name+" joined")
	case event.GetLeft() != nil && name != "":
		p.mark("←", dim, name+" left")
	case event.GetChat() != nil:
		p.line(p.pad() + paint(p.color, bold, event.GetChat().GetName()+":") + " " + event.GetChat().GetText())
	case event.GetStarted() != nil && event.GetStarted().GetReloaded():
		p.mark("↻", cyan, event.GetStarted().GetMod()+" reloaded")
	case event.GetStarted() != nil:
		p.mark("✓", green, event.GetStarted().GetMod()+" started")
	case event.GetLog() != nil:
		p.line(p.pad() + paint(p.color, dim, "["+event.GetLog().GetMod()+"]") + " " + event.GetLog().GetText())
	case event.GetMessage() != nil:
		p.line(p.pad() + paint(p.color, dim, "["+event.GetMessage().GetMod()+"]") + " " + shown(event.GetMessage()))
	case event.GetFailed() != nil:
		p.failed(event.GetFailed().GetMod(), event.GetFailed().GetError())
	case event.GetCalls() != nil:
		p.refused(event.GetCalls())
	}
}

// refused prints the calls the host refused in one report of a mod's calls,
// which a program can still read in full from the event stream.
func (p *printer) refused(calls *control.ModCalls) {
	var refused []string
	keyed := make(map[string]bool)
	for _, key := range calls.GetProfile().GetKeys() {
		if n := key.GetCounts().GetRefused(); n > 0 {
			keyed[key.GetMethod()] = true
			method := key.GetMethod()
			if method == "" {
				method = "unknown method"
			}
			refused = append(refused, fmt.Sprintf("%s %s ×%d", method, key.GetKey(), n))
		}
	}
	for _, method := range calls.GetProfile().GetMethods() {
		if n := method.GetCounts().GetRefused(); n > 0 && !keyed[method.GetMethod()] {
			refused = append(refused, fmt.Sprintf("%s ×%d", method.GetMethod(), n))
		}
	}
	if len(refused) != 0 {
		p.mark("!", yellow, calls.GetMod()+": the host refused "+strings.Join(refused, ", "))
	}
}

// name follows each player's name through joins and leaves, and returns the
// name of the player who joined or left in event.
func (p *printer) name(event *control.HostEvent) string {
	p.mu.Lock()
	defer p.mu.Unlock()
	switch {
	case event.GetJoined() != nil:
		if p.names == nil {
			p.names = make(map[int32]string)
		}
		p.names[event.GetJoined().GetSlot()] = event.GetJoined().GetName()
		return event.GetJoined().GetName()
	case event.GetLeft() != nil:
		name := p.names[event.GetLeft().GetSlot()]
		delete(p.names, event.GetLeft().GetSlot())
		return name
	default:
		return ""
	}
}

// failed prints a failure on standard error, naming the mod when there is
// one.
func (p *printer) failed(mod, text string) {
	if mod != "" {
		text = mod + ": " + text
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	_, _ = os.Stderr.WriteString(p.pad() + paint(p.errColor, red, "✗") + " " + text + "\n")
}

// indent moves each line of text two columns right.
func indent(text string) string {
	return "  " + strings.ReplaceAll(strings.TrimSuffix(text, "\n"), "\n", "\n  ") + "\n"
}

// shown describes a message as the player sees it.
func shown(message *control.PlayerMessage) string {
	switch message.GetKind() {
	case control.MessageKind_MESSAGE_KIND_CENTER:
		return "the player sees mid-screen: " + message.GetText()
	case control.MessageKind_MESSAGE_KIND_ANNOUNCEMENT:
		return "the player sees an announcement: " + message.GetTitle() + ": " + message.GetText()
	default:
		return "the player sees in chat: " + message.GetText()
	}
}

// tap adds watch to the watchers. Call it before the first report.
func (p *printer) tap(watch func(*cli.Event)) {
	p.watchers = append(p.watchers, watch)
}

// observe passes event to each watcher.
func (p *printer) observe(event *cli.Event) {
	for _, watch := range p.watchers {
		watch(event)
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

// build builds the project in mode, reporting the build's start and result.
// Tool output goes to standard error.
func (p *printer) build(ctx context.Context, opened *project.Project, mode project.Mode) error {
	p.building(opened.Dir)
	err := opened.Build(ctx, os.Stderr, mode)
	p.built(opened.Dir, err)
	return err
}
