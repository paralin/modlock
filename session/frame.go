package session

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"

	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm/manifest"
	"github.com/paralin/modlock/src/wasm/ui"
)

// FrameMod is the reserved mod name of the frame a session draws around the
// mods' interfaces. A host that frames the session itself, such as one with a
// HUD of its own, drops this tree.
const FrameMod = "modlock"

// Frame names a session in each player's top corners: "modlock" at the top
// left, in place of the game's own mark, and at the top right a debug line
// with each mod's name and version, how many times the mods reloaded, and
// Modlock's version. It reaches each player as an ordinary interface
// under FrameMod, so every renderer and every guest draws it.
type Frame struct {
	// version is Modlock's release.
	version string
	// mods are the built mods' directories.
	mods []string

	// mu guards the fields below.
	mu sync.Mutex
	// reloads counts the mods' reloads since the session started.
	reloads int
	// players holds each human player's slot.
	players map[int32]bool
}

// NewFrame returns the frame of a session of Modlock release version running
// the mods built at mods.
func NewFrame(version string, mods []string) *Frame {
	return &Frame{version: version, mods: mods, players: make(map[int32]bool)}
}

// Follow returns the frame of each player event changes, to pass on after
// event: a human who joins gets the frame, and a reload redraws it for
// everyone.
func (f *Frame) Follow(event *control.HostEvent) []*control.HostEvent {
	// Track the players and the reloads.
	f.mu.Lock()
	defer f.mu.Unlock()
	var slots []int32
	switch {
	case event.GetJoined() != nil:
		joined := event.GetJoined()
		if joined.GetSteamId() == 0 {
			delete(f.players, joined.GetSlot())
			break
		}
		f.players[joined.GetSlot()] = true
		slots = append(slots, joined.GetSlot())
	case event.GetLeft() != nil:
		delete(f.players, event.GetLeft().GetSlot())
	case event.GetStarted().GetReloaded():
		f.reloads++
		for slot := range f.players {
			slots = append(slots, slot)
		}
	}

	// Draw the frame for each changed player.
	if len(slots) == 0 {
		return nil
	}
	var events []*control.HostEvent
	change := &ui.Change{Reset_: true, Set: frameNodes(f.line())}
	for _, slot := range slots {
		update := &control.UiChanged{Mod: FrameMod, Slot: slot, Change: change}
		events = append(events, &control.HostEvent{Body: &control.HostEvent_Ui{Ui: update}})
	}
	return events
}

// line is the debug line, the game first: "Blind Draft Duel 0.1.1 · reload 3
// · modlock v0.1.67". It reads each mod's built mod.json, so a reload that
// changed a name or version shows it. Callers hold mu.
func (f *Frame) line() string {
	var parts []string
	for _, dir := range f.mods {
		mod, err := readManifest(dir)
		if err != nil {
			continue
		}
		parts = append(parts, strings.TrimSpace(mod.GetName()+" "+mod.GetVersion()))
	}
	parts = append(parts, "reload "+strconv.Itoa(f.reloads), "modlock "+f.version)
	return strings.Join(parts, " · ")
}

// readManifest reads the mod.json of the mod built at dir.
func readManifest(dir string) (*manifest.Manifest, error) {
	data, err := os.ReadFile(filepath.Join(dir, "mod.json"))
	if err != nil {
		return nil, err
	}
	mod := &manifest.Manifest{}
	return mod, mod.UnmarshalJSON(data)
}

// Frame styles: the mark sits where the game's own does, in the game's block
// capitals kept lowercase, and the line sits left of the game's 300px
// telemetry panel, so it never covers the FPS readout.
var (
	// frameEdge is the left inset of the game's own HUD along the left edge.
	frameEdge = float32(26)
	// telemetryWidth is the width of the game's top-right telemetry panel.
	telemetryWidth = float32(300)
	// markColor and lineColor are near white and a dimmer gray, as 0xRRGGBBAA.
	markColor = uint32(0xf2f2f2eb)
	lineColor = uint32(0xc8c8c8cc)
)

// frameNodes returns the frame's tree: a root that hides the game's corner
// mark, the "modlock" mark and the debug line.
func frameNodes(line string) []*ui.Node {
	return []*ui.Node{
		{Id: "", Kind: ui.Kind_KIND_PANEL, Children: []string{"mark", "line"}, HideHud: []ui.HudPart{ui.HudPart_HUD_PART_STATUS}},
		{Id: "mark", Kind: ui.Kind_KIND_LABEL, Text: "modlock", Style: &ui.Style{
			HorizontalAlign: ui.Align_ALIGN_START,
			VerticalAlign:   ui.Align_ALIGN_START,
			Margin:          &ui.Edges{Top: 16, Left: frameEdge},
			Face:            ui.Face_FACE_BLOCK,
			FontSize:        20,
			LetterSpacing:   1,
			Color:           markColor,
			Shadow:          true,
		}},
		{Id: "line", Kind: ui.Kind_KIND_LABEL, Text: line, Style: &ui.Style{
			HorizontalAlign: ui.Align_ALIGN_END,
			VerticalAlign:   ui.Align_ALIGN_START,
			Margin:          &ui.Edges{Top: 10, Right: telemetryWidth + 8},
			FontSize:        12,
			Color:           lineColor,
			Shadow:          true,
		}},
	}
}
