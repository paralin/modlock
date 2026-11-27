package ui

import (
	"slices"
	"strconv"

	"github.com/paralin/modlock/mod"
	uipb "github.com/paralin/modlock/src/wasm/ui"
)

// view is what the mod shows one player: its interface and its notices.
type view struct {
	// player is whom the view is for.
	player mod.Player
	// root is the mod's interface.
	root Node
	// hud lists the parts of the game's HUD to hide.
	hud []uipb.HudPart
	// cursor is true while the mod frees the player's mouse cursor.
	cursor bool
	// asks counts the mod's calls that freed the cursor.
	asks uint32
	// routes holds each route's name and value as JSON, in the order added.
	routes []route
	// cues are the last sounds played, which the renderer plays once each.
	cues []*uipb.Cue
	// timelines are the timelines the mod plays, in the order first played.
	timelines []*uipb.Timeline
	// bar takes clicks on the game's ability bar, while the mod asks for them.
	bar func(p mod.Player, slot int)
	// actions takes each phase of the game actions the mod follows.
	actions map[mod.GameAction]func(p mod.Player, phase mod.ActionPhase)
	// overlay takes each stock overlay that takes or gives back the input,
	// while the mod follows them.
	overlay func(p mod.Player, overlay mod.Overlay)
	// placement takes each report of where the player's screen places the
	// interface, while the mod follows placement.
	placement func(p mod.Player, placement Placement)
	// hudFrame is true while the interface is drawn in the HUD's frame.
	hudFrame bool
	// display is the abilities shown on Tab's rows and the ability bar.
	display []AbilityRow
	// notices are the toasts showing, oldest first.
	notices []notice
	// windows holds the window the player's screen shows of each list, by
	// the list's node id.
	windows map[string]listWindow
}

// route is one value a view sends its layouts under a name.
type route struct {
	// name is the route's name.
	name string
	// data is the route's value as JSON.
	data string
}

// notice is one toast, shown until the game clock passes until.
type notice struct {
	// id numbers the notice, which keys it in the stack.
	id int
	// text is what the notice says.
	text string
	// until is when the notice expires, on the game clock.
	until float64
}

// handlers are the handlers of one button or input; each is nil when unset.
type handlers struct {
	// press runs when the player presses a button.
	press func(p mod.Player)
	// text runs when the player types in an entry.
	text func(p mod.Player, text string)
	// number runs when the player drags a slider.
	number func(p mod.Player, value float32)
	// on runs when the player checks a toggle.
	on func(p mod.Player, on bool)
	// choice runs when the player picks an option.
	choice func(p mod.Player, index int)
	// wheel runs when the player turns the wheel over a slider.
	wheel func(p mod.Player, wheel *mod.Wheel)
	// hold runs as the player grabs, lets go of or loses a range's handle.
	hold func(p mod.Player, event *mod.RangeEvent)
	// row runs when the player chooses a list's row, with its key.
	row func(p mod.Player, key string)
	// bound runs a layout's bound panel's press, by the panel's id.
	bound map[string]func(p mod.Player)
}

// library holds the interface library's state: what the mod shows each
// player, the screens they hold, and the clock and counters they share.
type library struct {
	// views holds what the mod shows each player, by slot.
	views map[int32]*view
	// retained keeps the screens accepted by the host.
	retained retained
	// generations holds each slot's connection generation from the last
	// player list, to drop a view when another player takes the slot.
	generations map[int32]uint32
	// now is the game clock at the last frame, in seconds.
	now float64
	// nextNotice numbers the next notice.
	nextNotice int
	// nextCue numbers the next sound, so the renderer plays each once.
	nextCue uint32
	// nextRun numbers the next timeline run, so each play is a new run.
	nextRun uint32
	// sheets lists the mod's style sheets, in the order the mod added them.
	sheets []string
}

// lib is the library's state for the mod.
var lib = &library{
	views:       map[int32]*view{},
	retained:    retained{screens: map[int32]*screen{}},
	generations: map[int32]uint32{},
	nextCue:     1,
	nextRun:     1,
}

const (
	// maxCues is how many sounds the root keeps, so a player whose screens
	// come slower than the mod plays sounds misses none of the last few.
	maxCues = 8
	// maxNotices bounds the stack; a newer notice pushes out the oldest.
	maxNotices = 4
	// maxDisplay bounds the rows AbilityDisplay shows.
	maxDisplay = 64
)

// viewOf returns what the mod shows player, made empty on first use.
func (l *library) viewOf(p mod.Player) *view {
	v := l.views[p.Slot()]
	if v == nil {
		v = &view{player: p}
		l.views[p.Slot()] = v
	}
	return v
}

// draw sends a view through the default retained-screen component.
func (l *library) draw(v *view) bool {
	return l.retained.draw(v)
}

// send asks the host to apply change to the player.
func send(p mod.Player, change *uipb.Change) bool {
	return mod.ApplyUi(p, change) == nil
}

// notices stacks the notices at the lower left, newest at the bottom, clear of
// the game's health, weapon and spirit stats and the status icons above them.
func notices(stack []notice) Node {
	// Each notice slides in from the left and fades as it expires; the stack
	// fades with its last notice.
	motion := &Transition{Seconds: 0.25, Easing: EaseOut}
	enter := &Style{Opacity: new(float32(0)), Translate: []float32{-16, 0}}
	exit := &Style{Opacity: new(float32(0))}
	labels := make(Nodes, len(stack))
	for i, each := range stack {
		labels[i] = Label{
			Key: strconv.Itoa(each.id),
			Style: Style{
				HorizontalAlign: Left,
				Margin:          Edges{4, 0, 0, 0},
				Padding:         Edges{5, 10},
				Background:      "#101418d8",
				Color:           "#f2f2f2",
				FontSize:        16,
				BorderRadius:    4,
				Transition:      motion,
			},
			Enter: enter,
			Exit:  exit,
			Text:  each.text,
		}
	}
	return Panel{
		Style: Style{
			Width:           FitChildren,
			Height:          FitChildren,
			Flow:            Down,
			HorizontalAlign: Left,
			VerticalAlign:   Bottom,
			Margin:          Edges{0, 0, 260, 24},
			Transition:      motion,
		},
		Exit:     exit,
		Children: labels,
	}
}

// displayRows puts rows on the wire for viewer, with each player's name and
// hero as the Tab screen shows them now.
func displayRows(viewer mod.Player, rows []AbilityRow) []*uipb.AbilityRow {
	// Read the names once, and each hero's class from its id.
	if len(rows) == 0 {
		return nil
	}
	players, _ := mod.Players()
	wire := make([]*uipb.AbilityRow, len(rows))
	for i, row := range rows {
		w := &uipb.AbilityRow{Player: uint32(row.Player.Slot()), Own: row.Player.Slot() == viewer.Slot()}
		for _, c := range players {
			if c.GetPlayer() == row.Player.Slot() {
				// A Steam name is at most 32 characters, within the host's
				// 128 bytes.
				w.Name = string([]rune(c.GetName())[:min(32, len([]rune(c.GetName())))])
			}
		}
		if pawn, err := row.Player.Pawn(); err == nil && pawn != nil {
			for _, hero := range mod.Heroes {
				if hero.ID == pawn.GetHero() {
					w.Hero = hero.Name
				}
			}
		}

		// Each of the four slots shows its ability, or the game's own.
		for _, shown := range row.Abilities[:min(4, len(row.Abilities))] {
			a := &uipb.ShownAbility{}
			if shown != nil {
				if shown.Name != "" {
					a.Ability = mod.ItemID(shown.Name)
				}
				a.Image, a.Upgrades = shown.Image, shown.Upgrades
			}
			w.Abilities = append(w.Abilities, a)
		}
		wire[i] = w
	}
	return wire
}

// press runs the handler of the button or bound panel the player pressed.
func (l *library) press(event *mod.UiPressEvent) {
	// A click on the game's ability bar names its slot, which no button's id
	// does.
	p, node := mod.PlayerOf(event.GetPlayer()), event.GetNode()
	if slot, ok := abilitySlot(node); ok {
		if v := l.views[p.Slot()]; v != nil && v.bar != nil {
			v.bar(p, slot)
		}
		return
	}

	// A layout's press names its bound panel; a button's names none.
	callbacks := l.retained.lookup(p.Slot(), node)
	h := callbacks.press
	if action := event.GetAction(); action != "" {
		h = callbacks.bound[action]
	}
	if h != nil {
		h(p)
	}
}

// act runs the handler of the game action the player pressed.
func (l *library) act(event *mod.UiActionEvent) {
	p := mod.PlayerOf(event.GetPlayer())
	if v := l.views[p.Slot()]; v != nil {
		if h := v.actions[event.GetAction()]; h != nil {
			h(p, event.GetPhase())
		}
	}
}

// overlaid runs the handler of the mod that follows the player's overlays.
func (l *library) overlaid(event *mod.UiOverlayEvent) {
	p := mod.PlayerOf(event.GetPlayer())
	if v := l.views[p.Slot()]; v != nil && v.overlay != nil {
		v.overlay(p, event.GetOverlay())
	}
}

// placed runs the handler of the mod that follows the player's placement.
func (l *library) placed(event *mod.UiPlacementEvent) {
	p := mod.PlayerOf(event.GetPlayer())
	if v := l.views[p.Slot()]; v != nil && v.placement != nil {
		measured := make(map[string]*mod.Bounds, len(event.GetMeasured()))
		for _, each := range event.GetMeasured() {
			measured[each.GetName()] = each.GetBounds()
		}
		v.placement(p, Placement{Screen: event.GetScreen(), Hud: event.GetHud(), Measured: measured})
	}
}

// frame returns the frame a root is drawn in.
func frame(hud bool) uipb.Frame {
	if hud {
		return uipb.Frame_FRAME_HUD
	}
	return uipb.Frame_FRAME_SCREEN
}

// abilitySlot returns the slot a click on the game's ability bar names.
func abilitySlot(node string) (int, bool) {
	if len(node) == len("ability:0") && node[:len("ability:")] == "ability:" && node[8] >= '0' && node[8] <= '3' {
		return int(node[8] - '0'), true
	}
	return 0, false
}

// change runs the handler of the input the player changed, with its value as
// the host checked it.
func (l *library) change(event *mod.UiInputEvent) {
	// Find the input's handlers on the screen the player holds.
	p := mod.PlayerOf(event.GetPlayer())
	h := l.retained.lookup(p.Slot(), event.GetNode())

	// Each value goes to the handler of its type.
	switch value := event.GetValue().(type) {
	case *mod.UiInputEventText:
		if h.text != nil {
			h.text(p, value.Text)
		}
	case *mod.UiInputEventNumber:
		if h.number != nil {
			h.number(p, value.Number)
		}
	case *mod.UiInputEventOn:
		if h.on != nil {
			h.on(p, value.On)
		}
	case *mod.UiInputEventChoice:
		if h.choice != nil {
			h.choice(p, int(value.Choice))
		}
	case *mod.UiInputEventWheel:
		if h.wheel != nil {
			h.wheel(p, value.Wheel)
		}
	case *mod.UiInputEventRange:
		if h.hold != nil {
			h.hold(p, value.Range)
		}
	case *mod.UiInputEventRow:
		if h.row != nil {
			h.row(p, value.Row)
		}
	}
}

// tick advances the clock to the frame at seconds and fades expiring notices.
func (l *library) tick(seconds float64) {
	l.now = seconds
	for _, v := range l.views {
		// Redraw only a view that lost a notice.
		before := len(v.notices)
		if before == 0 {
			continue
		}
		v.notices = slices.DeleteFunc(v.notices, func(n notice) bool { return n.until <= l.now })
		if len(v.notices) != before {
			l.draw(v)
		}
	}
}

// returned sends a returning player their whole interface, which their client
// lost while they were away.
func (l *library) returned(event *mod.PlayerReturnedEvent) {
	slot := event.GetConnection().GetPlayer()
	if v := l.views[slot]; v != nil {
		l.retained.invalidate(slot)
		v.windows = nil
		l.draw(v)
	}
}

// forget drops the view and screen of each slot whose player left or was
// replaced, so a new player in the slot starts with nothing shown.
func (l *library) forget(players []*mod.Connection) {
	// Find each slot whose connection is gone or new.
	current := make(map[int32]uint32, len(players))
	for _, c := range players {
		current[c.GetPlayer()] = c.GetGeneration()
	}
	for slot, generation := range l.generations {
		if seen, ok := current[slot]; !ok || seen != generation {
			delete(l.views, slot)
			l.retained.invalidate(slot)
		}
	}
	l.generations = current
}
