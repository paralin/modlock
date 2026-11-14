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
	// bar takes clicks on the game's ability bar, while the mod asks for them.
	bar func(p mod.Player, slot int)
	// display is the abilities shown on Tab's rows and the ability bar.
	display []AbilityRow
	// notices are the toasts showing, oldest first.
	notices []notice
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

// screen is what one player's interface holds, as the host last accepted it.
type screen struct {
	// nodes maps each node's id to its encoding, to find what changed.
	nodes map[string]*uipb.Node
	// order lists the nodes in the order laid out, children before parents.
	order []*uipb.Node
	// ids maps each element's place in the tree to its node's id. A place is
	// its parent's place, then "/" for the children, ",index" or ",=key" in a
	// list, and ">" for what a component rendered.
	ids map[string]string
	// handlers maps each button's and input's id to its handlers.
	handlers map[string]handlers
	// next numbers the next new node, so an id is never reused.
	next int
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
}

// layout holds one screen while place lays it out.
type layout struct {
	// previous is the screen the player holds, whose ids the layout keeps.
	previous *screen
	// screen is the screen being laid out.
	screen *screen
}

// library holds the interface library's state: what the mod shows each
// player, the screens they hold, and the clock and counters they share.
type library struct {
	// views holds what the mod shows each player, by slot.
	views map[int32]*view
	// screens holds each player's interface by slot.
	screens map[int32]*screen
	// generations holds each slot's connection generation from the last
	// player list, to drop a view when another player takes the slot.
	generations map[int32]uint32
	// now is the game clock at the last frame, in seconds.
	now float64
	// nextNotice numbers the next notice.
	nextNotice int
	// nextCue numbers the next sound, so the renderer plays each once.
	nextCue uint32
	// sheets lists the mod's style sheets, in the order the mod added them.
	sheets []string
}

// lib is the library's state for the mod.
var lib = &library{
	views:       map[int32]*view{},
	screens:     map[int32]*screen{},
	generations: map[int32]uint32{},
	nextCue:     1,
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
func viewOf(p mod.Player) *view {
	v := lib.views[p.Slot()]
	if v == nil {
		v = &view{player: p}
		lib.views[p.Slot()] = v
	}
	return v
}

// draw sends what changed in the player's interface and notices.
func draw(v *view) bool {
	// Render the player's view and any notices against the last screen sent.
	root := Nodes{v.root, nil}
	if len(v.notices) != 0 {
		root[1] = notices(v.notices)
	}
	slot := v.player.Slot()
	previous := lib.screens[slot]
	display := displayRows(v.player, v.display)
	next := render(previous, v, root, display)
	change := diff(previous, next)

	// Send the change; a failed send means the player may have left, so send
	// the screen whole.
	if change != nil && !send(v.player, change) {
		whole := render(nil, v, root, display)
		if !send(v.player, diff(nil, whole)) {
			delete(lib.screens, slot)
			return false
		}
		lib.screens[slot] = whole
		return true
	}

	// Keep the screen the player now holds.
	lib.screens[slot] = next
	return true
}

// render lays root out as nodes, keeping the ids of elements that stayed in
// place, beside a node for each route.
func render(previous *screen, v *view, root Node, display []*uipb.AbilityRow) *screen {
	// The root is the full-screen panel the elements hang from, and it lists
	// the parts of the game's HUD to hide, whether the cursor is free, the
	// mod's sheets and its last sounds.
	l := &layout{previous: previous, screen: &screen{
		nodes:    map[string]*uipb.Node{},
		ids:      map[string]string{},
		handlers: map[string]handlers{},
	}}
	if previous != nil {
		l.screen.next = previous.next
	}
	l.add(&uipb.Node{
		Id:                "",
		Kind:              uipb.Kind_KIND_PANEL,
		Children:          l.place(root, "/", nil),
		HideHud:           slices.Clone(v.hud),
		FreeCursor:        v.cursor,
		FreeCursorAsks:    v.asks,
		Sheets:            slices.Clone(lib.sheets),
		Cues:              slices.Clone(v.cues),
		AbilityBarPresses: v.bar != nil,
		AbilityDisplay:    display,
	})

	// A route's id holds a colon, which no element's id does.
	for _, r := range v.routes {
		l.add(&uipb.Node{Id: "route:" + r.name, Kind: uipb.Kind_KIND_ROUTE, Route: r.name, Data: r.data})
	}
	return l.screen
}

// add records a node.
func (l *layout) add(n *uipb.Node) {
	l.screen.nodes[n.Id] = n
	l.screen.order = append(l.screen.order, n)
}

// id returns the node id for the element at place.
func (l *layout) id(place string) string {
	// Keep the id the place had on the screen the player holds.
	found, ok := "", false
	if l.previous != nil {
		found, ok = l.previous.ids[place]
	}
	if !ok {
		found = strconv.FormatInt(int64(l.screen.next), 36)
		l.screen.next++
	}
	l.screen.ids[place] = found
	return found
}

// place lays out child at place at and appends its nodes' ids to ids.
func (l *layout) place(child Node, at string, ids []string) []string {
	switch c := child.(type) {
	case nil:
		return ids
	case Nodes:
		// A keyed child keeps its place by key, the rest by index.
		for i, item := range c {
			where := at + "," + strconv.Itoa(i)
			if k, ok := item.(keyed); ok && k.key() != "" {
				where = at + ",=" + k.key()
			}
			ids = l.place(item, where, ids)
		}
		return ids
	case Text:
		n := &uipb.Node{Id: l.id(at), Kind: uipb.Kind_KIND_LABEL, Text: string(c)}
		l.add(n)
		return append(ids, n.Id)
	case element:
		n := &uipb.Node{Id: l.id(at)}
		c.draw(l, n, at)
		l.add(n)
		return append(ids, n.Id)
	}

	// A component's nodes hang under its place.
	return l.place(child.Render(), at+">", ids)
}

// diff returns the change from previous to next, or nil when nothing changed.
func diff(previous *screen, next *screen) *uipb.Change {
	// Send each node whose encoding changed, and remove each node gone.
	var set []*uipb.Node
	for _, n := range next.order {
		if previous == nil || !previous.nodes[n.Id].EqualVT(n) {
			set = append(set, n)
		}
	}
	var removed []string
	if previous != nil {
		for _, n := range previous.order {
			if _, ok := next.nodes[n.Id]; !ok {
				removed = append(removed, n.Id)
			}
		}
	}
	if previous != nil && len(set) == 0 && len(removed) == 0 {
		return nil
	}
	return &uipb.Change{Reset_: previous == nil, Set: set, Removed: removed}
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

// press runs the handler of the button the player pressed.
func press(p mod.Player, node string) {
	// A click on the game's ability bar names its slot, which no button's id
	// does.
	if slot, ok := abilitySlot(node); ok {
		if v := lib.views[p.Slot()]; v != nil && v.bar != nil {
			v.bar(p, slot)
		}
		return
	}
	if s := lib.screens[p.Slot()]; s != nil {
		if h := s.handlers[node].press; h != nil {
			h(p)
		}
	}
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
func change(event *mod.UiInputEvent) {
	// Find the input's handlers on the screen the player holds.
	p := mod.PlayerOf(event.GetPlayer())
	s := lib.screens[p.Slot()]
	if s == nil {
		return
	}
	h := s.handlers[event.GetNode()]

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
	}
}

// tick advances the clock to the frame at seconds and fades expiring notices.
func tick(seconds float64) {
	lib.now = seconds
	for _, v := range lib.views {
		// Redraw only a view that lost a notice.
		before := len(v.notices)
		if before == 0 {
			continue
		}
		v.notices = slices.DeleteFunc(v.notices, func(n notice) bool { return n.until <= lib.now })
		if len(v.notices) != before {
			draw(v)
		}
	}
}

// forget drops the view and screen of each slot whose player left or was
// replaced, so a new player in the slot starts with nothing shown.
func forget(players []*mod.Connection) {
	// Find each slot whose connection is gone or new.
	current := make(map[int32]uint32, len(players))
	for _, c := range players {
		current[c.GetPlayer()] = c.GetGeneration()
	}
	for slot, generation := range lib.generations {
		if seen, ok := current[slot]; !ok || seen != generation {
			delete(lib.views, slot)
			delete(lib.screens, slot)
		}
	}
	lib.generations = current
}
