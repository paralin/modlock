package ui

import (
	"slices"
	"strings"

	"github.com/aperturerobotics/fastjson"
	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// Show replaces the interface the mod shows player with root. A player who
// left and a mod that restarted show nothing until the next call.
func Show(p mod.Player, root Node) bool {
	v := viewOf(p)
	v.root = root
	return draw(v)
}

// Hide removes the interface, the routes and the notices the mod shows player.
func Hide(p mod.Player) bool {
	delete(lib.views, p.Slot())
	delete(lib.screens, p.Slot())
	return send(p, &uipb.Change{Reset_: true})
}

// HideHud hides parts of the game's own HUD from player until the next call;
// no parts shows them all again.
func HideHud(p mod.Player, parts ...HudPart) bool {
	v := viewOf(p)
	v.hud = v.hud[:0]
	for _, part := range parts {
		v.hud = append(v.hud, hudPart(part))
	}
	return draw(v)
}

// FreeCursor shows player's mouse cursor while free is true, so they can click
// the mod's panels, such as a pick screen, without holding Tab. A false free
// puts the cursor away and gives the mouse back to aiming. While the cursor is
// up, a click on one of the mod's panels goes to that panel, and the hero does
// not shoot. A click anywhere else fires the weapon, and the camera and
// movement keep working. So a screen that must never fire calls p.BlockInput
// for the fire buttons, and releases them when it hides. The player can also
// put the cursor away with the game's own keys. It then stays away when the mod
// draws a new screen, until the next call with true shows it again.
func FreeCursor(p mod.Player, free bool) bool {
	v := viewOf(p)
	v.cursor = free
	if free {
		v.asks++
	}
	return draw(v)
}

// CursorFree reports whether player's mouse cursor is up now, whether the mod
// freed it with FreeCursor or the player freed it with the game's own keys. ok
// is false while the player does not have a hero, because the game keeps the
// cursor's state on the hero.
func CursorFree(p mod.Player) (free, ok bool) {
	// Read the flag from the player's hero.
	pawn, err := p.Pawn()
	if err != nil || pawn == nil {
		return false, false
	}
	value, err := mod.ReadField(pawn.GetEntity(), "CCitadelPlayer_MovementServices", "m_bHasFreeCursor", mod.FieldTypeBool)
	free, ok = value.(bool)
	return free, ok && err == nil
}

// AbilityBarPresses calls onPress with the slot, 0 to 3, each time player
// clicks the icon or upgrade pips of an ability on the game's own ability bar,
// as the game does when it trains one with Tab held; a nil onPress stops the
// clicks reaching the mod. The game answers a click from the hero's own
// abilities, and sends nothing for an ability the mod replaced, so a mod that
// replaces abilities answers here. Clicks reach the mod only while its screen
// holds the player's HUD.
func AbilityBarPresses(p mod.Player, onPress func(p mod.Player, slot int)) bool {
	v := viewOf(p)
	v.bar = onPress
	return draw(v)
}

// FollowAction calls onPhase with each phase of action, a game action such
// as mod.GameActionScoreboard, each time player uses it, on whichever key
// they bound it to: pressed once, then released or cancelled once. The game
// still does what the action does. A press made while a stock overlay, such
// as chat, the shop or the escape menu, has the player's input reports
// nothing, and one held as such an overlay opens is cancelled. A nil onPhase
// stops following. Actions reach the mod only while its screen holds the
// player's HUD.
func FollowAction(p mod.Player, action mod.GameAction, onPhase func(p mod.Player, phase mod.ActionPhase)) bool {
	v := viewOf(p)
	if onPhase == nil {
		delete(v.actions, action)
	} else {
		if v.actions == nil {
			v.actions = map[mod.GameAction]func(mod.Player, mod.ActionPhase){}
		}
		v.actions[action] = onPhase
	}
	return draw(v)
}

// FollowOverlay calls onChange with the stock overlay, such as
// mod.OverlayChat, mod.OverlayShop or mod.OverlayEscapeMenu, that holds
// player's input: first the current one, then each time a different one
// takes it, or mod.OverlayNone as the game gets it back. When several are
// open it names the first of chat, the shop, the build editor, the item
// draft, a popup and the escape menu. A nil onChange stops following.
func FollowOverlay(p mod.Player, onChange func(p mod.Player, overlay mod.Overlay)) bool {
	v := viewOf(p)
	v.overlay = onChange
	return draw(v)
}

// Placement is where a player's screen places the interface, in layout
// units from the screen's top left, where the screen is 1080 units tall.
type Placement struct {
	// Screen is the whole screen, at 0, 0.
	Screen *mod.Bounds
	// Hud is the HUD's frame: the screen, or the middle 16:9 of a wider
	// screen whose player keeps the HUD there.
	Hud *mod.Bounds
	// Measured holds the box of each element whose Measure names it, once
	// the screen has laid it out.
	Measured map[string]*mod.Bounds
}

// FollowPlacement calls onPlace with where player's screen places the
// interface: the whole placement first, then again each time any part of it
// moves, such as when the player changes resolution or an element's layout
// settles. Elements that share a Measure report one box. A nil onPlace stops
// following.
func FollowPlacement(p mod.Player, onPlace func(p mod.Player, placement Placement)) bool {
	v := viewOf(p)
	v.placement = onPlace
	return draw(v)
}

// InHudFrame draws player's interface inside the HUD's frame instead of the
// whole screen. The frame is the screen itself, except on a screen wider
// than 16:9 where the player keeps the HUD in its middle 16:9, so an
// interface aligned to the frame's edges lines up with the game's HUD.
func InHudFrame(p mod.Player, on bool) bool {
	v := viewOf(p)
	v.hudFrame = on
	return draw(v)
}

// AbilityDisplay shows viewer each row's abilities in place of the ones the
// game shows for that player's hero: on the player's row of the game's Tab
// screen, and for viewer's own row on viewer's ability bar too, in the game's
// frame, ring and pips, with the game's ability window over each. Without
// rows the game's own come back, as they do when viewer leaves or the session
// ends. Rows match the Tab screen's by the player's name and else by the
// hero's, so each row's player should have a hero.
func AbilityDisplay(viewer mod.Player, rows ...AbilityRow) bool {
	v := viewOf(viewer)
	v.display = slices.Clone(rows[:min(len(rows), maxDisplay)])
	return draw(v)
}

// Sheet adds the style sheet at path in the mod's content styles directory,
// such as "hud.css", to every interface the mod shows, after the sheets added
// before it. Adding a sheet again changes nothing. A path is letters, digits,
// '_', '-' and '.' in segments separated by '/', ending in ".css"; another
// adds nothing and returns false.
func Sheet(path string) bool {
	// Refuse a path the renderer cannot load, and keep a sheet added before.
	if !strings.HasSuffix(path, ".css") {
		return false
	}
	for part := range strings.SplitSeq(path, "/") {
		if !plainName(part) || part == ".." {
			return false
		}
	}
	if slices.Contains(lib.sheets, path) {
		return true
	}

	// Redraw every interface with the new sheet.
	lib.sheets = append(lib.sheets, path)
	for _, v := range lib.views {
		draw(v)
	}
	return true
}

// Route sends player the JSON text value under name, for every layout of the
// mod to read with Modlock.route, until the next call; "" removes it. A name
// is letters, digits, '_', '-' and '.'. A route with another name, or a value
// that is not JSON, sends nothing and returns false.
func Route(p mod.Player, name, value string) bool {
	// Refuse a name no attribute can carry and a value that is not JSON.
	if !plainName(name) || (value != "" && fastjson.Validate(value) != nil) {
		return false
	}

	// Store the value in the route's place, or forget the route, and redraw.
	v := viewOf(p)
	at := slices.IndexFunc(v.routes, func(r route) bool { return r.name == name })
	switch {
	case value == "" && at >= 0:
		v.routes = slices.Delete(v.routes, at, at+1)
	case value == "":
	case at >= 0:
		v.routes[at].data = value
	default:
		v.routes = append(v.routes, route{name: name, data: value})
	}
	return draw(v)
}

// plainName reports whether name is letters, digits, '_', '-' and '.' alone.
func plainName(name string) bool {
	if name == "" {
		return false
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' || c == '-' || c == '.') {
			return false
		}
	}
	return true
}

// Toast shows player text in a notice at the lower left for four seconds.
// Notices stack, newest last, slide in, and fade as they expire.
func Toast(p mod.Player, text string) bool {
	return ToastFor(p, text, 4)
}

// ToastFor shows player text in a notice for seconds, as Toast does.
func ToastFor(p mod.Player, text string, seconds float64) bool {
	// Add the notice, keeping only the newest few.
	v := viewOf(p)
	v.notices = append(v.notices, notice{id: lib.nextNotice, text: text, until: lib.now + seconds})
	lib.nextNotice++
	if len(v.notices) > maxNotices {
		v.notices = slices.Delete(v.notices, 0, len(v.notices)-maxNotices)
	}
	return draw(v)
}

// Sound plays the game's sound event sound, such as "UI.ItemDraft.Start", in
// player's game alone, once; seconds above 0 cuts it after that long, keeping
// only its start. It plays with the next screen the player reads, and not at
// all if the player reads it only after the mod's next few sounds.
// PlayTimeline times a run of sounds, such as a wheel's ticks.
func Sound(p mod.Player, sound string, seconds float32) bool {
	// Add the sound, keeping only the newest few.
	v := viewOf(p)
	v.cues = append(v.cues, &uipb.Cue{Number: lib.nextCue, Sound: sound, Seconds: seconds})
	lib.nextCue++
	if len(v.cues) > maxCues {
		v.cues = slices.Delete(v.cues, 0, len(v.cues)-maxCues)
	}
	return draw(v)
}

// TimelineStep is one moment of a timeline, At seconds from its start, 0 to
// 60. From then on the elements whose Measure the step names take its look:
// Class adds classes from the mod's sheets, separated by spaces, and Style is
// laid over each element's own, setting, as a hover does, only what the
// element's style sets or what has a resting value. A step's look replaces
// the look an earlier step gave the same measure. Sound plays the game's
// sound event, cut after Seconds unless Seconds is zero.
type TimelineStep struct {
	At      float32
	Measure string
	Class   string
	Style   *Style
	Sound   string
	Seconds float32
}

// Timeline is a timed presentation. Its steps run on the player's own time,
// or, with Game set, hold while the game is paused.
type Timeline struct {
	Game  bool
	Steps []TimelineStep
}

// PlayTimeline starts timeline id in player's interface, or starts it again
// from its start, taking back the looks its last run gave. A finished
// timeline keeps its last looks until it is played again or cancelled. A
// renderer that reloads mid-run shows the run's last looks and plays none of
// its sounds.
func PlayTimeline(p mod.Player, id string, t Timeline) bool {
	// Encode the run, numbered after every earlier one.
	wire := &uipb.Timeline{Id: id, Run: lib.nextRun}
	lib.nextRun++
	if t.Game {
		wire.Clock = uipb.TimelineClock_TIMELINE_CLOCK_GAME
	}
	for _, step := range t.Steps {
		wire.Steps = append(wire.Steps, &uipb.TimelineStep{
			At:      step.At,
			Measure: step.Measure,
			Classes: classes(step.Class),
			Style:   wireStyle(step.Style),
			Sound:   step.Sound,
			Seconds: step.Seconds,
		})
	}

	// Replace the timeline of that id, or add it.
	v := viewOf(p)
	at := slices.IndexFunc(v.timelines, func(each *uipb.Timeline) bool { return each.Id == id })
	if at < 0 {
		v.timelines = append(v.timelines, wire)
	} else {
		v.timelines[at] = wire
	}
	return draw(v)
}

// CancelTimeline stops timeline id in player's interface, taking back its
// looks and playing none of its sounds still to come.
func CancelTimeline(p mod.Player, id string) bool {
	v := viewOf(p)
	at := slices.IndexFunc(v.timelines, func(each *uipb.Timeline) bool { return each.Id == id })
	if at < 0 {
		return true
	}
	v.timelines = slices.Delete(v.timelines, at, at+1)
	return draw(v)
}
