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

// FreeCursor shows player's mouse cursor without Tab held while free is true,
// so they can click the mod's panels, such as a pick screen; false gives the
// mouse back to aiming. Each call with true shows the cursor again, even when
// the player put it away with the game's own keys; a screen that changes
// without a call leaves their choice be. The game does not stop the player's
// shots, camera or movement while the cursor is up: a screen that must not
// fire a weapon blocks the buttons it takes.
func FreeCursor(p mod.Player, free bool) bool {
	v := viewOf(p)
	v.cursor = free
	if free {
		v.asks++
	}
	return draw(v)
}

// CursorFree reports whether player's mouse cursor is up now, whether the mod
// freed it with FreeCursor or the player did with the game's own keys. ok is
// false while the player has no hero to read.
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
