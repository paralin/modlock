package mod

import (
	"slices"
	"testing"
)

// TestRegistryDeliveryTiming checks registration through the default API across
// Start, deferred events, and a frame callback that registers another callback.
func TestRegistryDeliveryTiming(t *testing.T) {
	// Isolate the module's default callbacks and game clock.
	held, heldClock := defaultRegistry, clock
	defaultRegistry = Registry{}
	resetClock(t)
	t.Cleanup(func() { defaultRegistry, clock = held, heldClock })
	var runs []string
	record := func(name string) { runs = append(runs, name) }
	OnStart(func([]string) {
		record("start")
		OnDamage(func(*DamageEvent) *DamageResult { return nil })
		OnStart(func([]string) { record("late-start") })
	})
	OnFrame(func(*FrameEvent) {
		record("frame")
		OnFrame(func(*FrameEvent) { record("late-frame") })
	})

	// Start's range keeps its initial length; its subscription sees new handlers.
	result, err := registered.Start(&StartEvent{})
	if err != nil || !result.GetDamage() || !slices.Equal(runs, []string{"start"}) {
		t.Fatalf("start: %v, %v, %v", result, err, runs)
	}
	Command("hello", func(p Player, args string) {
		if p != PlayerOf(2) || args != "world" {
			t.Fatalf("command: %v %q", p, args)
		}
		record("command")
	})
	OnChat(func(*ChatEvent) { record("chat") })
	OnUiPress(func(*UiPressEvent) { record("press") })

	// Command keeps the old named handler; chat and UI see late registrations.
	claim, err := registered.Command(&CommandEvent{Player: 2, Line: " hello  world "})
	if err != nil || !claim.GetClaimed() {
		t.Fatalf("claim: %v, %v", claim, err)
	}
	_ = registered.Chat(&ChatEvent{})
	_ = registered.UiPress(&UiPressEvent{})
	Command("hello", func(Player, string) { record("replacement") })
	OnChat(func(*ChatEvent) { record("late-chat") })
	OnUiPress(func(*UiPressEvent) { record("late-press") })
	if !slices.Equal(runs, []string{"start"}) {
		t.Fatalf("ran before frame: %v", runs)
	}

	// Timers run first, and a frame range excludes its own late registration.
	_ = registered.Frame(&FrameEvent{})
	want := []string{"start", "command", "chat", "late-chat", "press", "late-press", "frame"}
	if !slices.Equal(runs, want) {
		t.Fatalf("first frame: %v", runs)
	}
	runs = nil
	_ = registered.Frame(&FrameEvent{})
	if !slices.Equal(runs, []string{"frame", "late-frame"}) {
		t.Fatalf("second frame: %v", runs)
	}
}

// TestRegistryFoldsAndCompletion checks distinct fold contracts and reentrant
// duplicate replies without a host service or a player-data operation.
func TestRegistryFoldsAndCompletion(t *testing.T) {
	// A zero Registry accepts named replacements and reports missing services.
	var registry Registry
	registry.RegisterService("echo", func(string, []byte) ([]byte, error) { return []byte("old"), nil })
	registry.RegisterService("echo", func(string, []byte) ([]byte, error) { return []byte("new"), nil })
	reply, err := registry.Serve(&ServiceCall{Service: "echo"})
	if err != nil || string(reply.GetPayload()) != "new" {
		t.Fatalf("service: %v, %v", reply, err)
	}
	if _, err := registry.Serve(&ServiceCall{Service: "missing"}); err == nil {
		t.Fatal("missing service accepted")
	}

	// Damage carries amounts forward and stops at the first block.
	amount := float32(7)
	registry.OnDamage(func(*DamageEvent) *DamageResult { return &DamageResult{Amount: &amount} })
	registry.OnDamage(func(hit *DamageEvent) *DamageResult {
		if hit.GetAmount() != amount {
			t.Fatalf("damage amount: %v", hit)
		}
		blocked := true
		return &DamageResult{Block: &blocked}
	})
	registry.OnDamage(func(*DamageEvent) *DamageResult { t.Fatal("ran after block"); return nil })
	damage, err := registry.Damage(&DamageEvent{})
	if err != nil || !damage.GetBlock() {
		t.Fatalf("damage: %v, %v", damage, err)
	}

	// Ping combines slots without duplicates while preserving first occurrence.
	registry.OnPing(func(*PingEvent) *PingResult { return &PingResult{MuteVoice: []int32{2, 1}} })
	registry.OnPing(func(*PingEvent) *PingResult { return &PingResult{MuteVoice: []int32{1, 3}} })
	ping, err := registry.Ping(&PingEvent{})
	if err != nil || !slices.Equal(ping.GetMuteVoice(), []int32{2, 1, 3}) {
		t.Fatalf("ping: %v, %v", ping, err)
	}

	// A refused host start still completes after CallServiceLater returns.
	heldClock := clock
	resetClock(t)
	t.Cleanup(func() { clock = heldClock })
	refused := 0
	var refusedError error
	CallServiceLater("missing", "method", nil, 0, func(_ []byte, err error) {
		refused++
		refusedError = err
	})
	if refused != 0 {
		t.Fatal("refused call completed before returning")
	}
	frame(0)
	if refused != 1 || refusedError == nil {
		t.Fatalf("refused call: %d, %v", refused, refusedError)
	}

	// A seeded successful host call is removed before its callback can reenter.
	completions := 0
	event := &ServiceRepliedEvent{Call: 9, Payload: []byte("answer")}
	pending := serviceCalls{waiting: map[uint64]func([]byte, error){}}
	pending.waiting[9] = func(reply []byte, err error) {
		completions++
		if err != nil || string(reply) != "answer" {
			t.Fatalf("completion: %q, %v", reply, err)
		}
		_ = pending.ServiceReplied(event)
	}
	_ = pending.ServiceReplied(event)
	_ = pending.ServiceReplied(event)
	if completions != 1 {
		t.Fatalf("completed %d times", completions)
	}
}
