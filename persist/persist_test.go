package persist

import (
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/paralin/modlock/src/wasm"
	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// stats is the record type the tests keep: a prop hunt mod's player stats.
var stats = &persistpb.RecordType{
	Name:    "stats",
	Writers: persistpb.Writers_WRITERS_SERVERS,
	Fields: map[string]*persistpb.Field{
		"roundsWon": {Kind: persistpb.Field_KIND_COUNT},
		"score":     {Kind: persistpb.Field_KIND_NUMBER},
		"bestHide":  {Kind: persistpb.Field_KIND_MAX},
		"title":     {Kind: persistpb.Field_KIND_STRING, MaxBytes: 8},
		"unlocks":   {Kind: persistpb.Field_KIND_TAGS},
	},
}

// manifest declares stats and the persist service.
func manifest() *wasm.Manifest {
	return &wasm.Manifest{
		Slug:     "prophunt",
		Services: []*wasm.Service{{Name: Service}},
		Persist:  []*persistpb.RecordType{stats},
	}
}

func add(field string, n float64) *persistpb.Op {
	return &persistpb.Op{Field: field, Op: &persistpb.Op_Add{Add: n}}
}

func setText(field, text string) *persistpb.Op {
	return &persistpb.Op{Field: field, Op: &persistpb.Op_Set{Set: &persistpb.Value{Value: &persistpb.Value_Text{Text: text}}}}
}

func insert(field, tag string) *persistpb.Op {
	return &persistpb.Op{Field: field, Op: &persistpb.Op_Insert{Insert: tag}}
}

// refused fails t unless err is a Refusal containing text.
func refused(t *testing.T, err error, text string) {
	t.Helper()
	var refusal Refusal
	if !errors.As(err, &refusal) || !strings.Contains(err.Error(), text) {
		t.Fatalf("want a refusal naming %q, got %v", text, err)
	}
}

func TestValidate(t *testing.T) {
	if err := Validate(manifest()); err != nil {
		t.Fatal(err)
	}
	cases := map[string]func(*wasm.Manifest){
		"no persist service": func(m *wasm.Manifest) { m.Services = nil },
		"no record types":    func(m *wasm.Manifest) { m.Persist = nil },
		"twice": func(m *wasm.Manifest) {
			m.Persist = append(m.Persist, stats)
		},
		"name": func(m *wasm.Manifest) {
			m.Persist = []*persistpb.RecordType{{Name: "9lives", Writers: persistpb.Writers_WRITERS_SERVERS, Fields: stats.Fields}}
		},
		"writers": func(m *wasm.Manifest) {
			m.Persist = []*persistpb.RecordType{{Name: "stats", Fields: stats.Fields}}
		},
		"no fields": func(m *wasm.Manifest) {
			m.Persist = []*persistpb.RecordType{{Name: "stats", Writers: persistpb.Writers_WRITERS_SERVERS}}
		},
		"kind": func(m *wasm.Manifest) {
			m.Persist = []*persistpb.RecordType{{Name: "stats", Writers: persistpb.Writers_WRITERS_SERVERS, Fields: map[string]*persistpb.Field{"a": {}}}}
		},
		"maxBytes on a count": func(m *wasm.Manifest) {
			m.Persist = []*persistpb.RecordType{{Name: "stats", Writers: persistpb.Writers_WRITERS_SERVERS, Fields: map[string]*persistpb.Field{"a": {Kind: persistpb.Field_KIND_COUNT, MaxBytes: 4}}}}
		},
	}
	for name, change := range cases {
		m := manifest()
		change(m)
		if Validate(m) == nil {
			t.Errorf("%s: want an error", name)
		}
	}
	m := manifest()
	for i := range TypeLimit {
		m.Persist = append(m.Persist, &persistpb.RecordType{Name: "t" + strconv.Itoa(i), Writers: persistpb.Writers_WRITERS_SERVERS, Fields: stats.Fields})
	}
	if Validate(m) == nil {
		t.Error("too many types: want an error")
	}
}

func TestApplyRules(t *testing.T) {
	// Each kind takes its own operations.
	next, repeated, err := Apply(stats, nil, []*persistpb.Op{
		add("roundsWon", 2),
		add("score", -1.5),
		{Field: "bestHide", Op: &persistpb.Op_Max{Max: 30}},
		{Field: "bestHide", Op: &persistpb.Op_Max{Max: 12}},
		setText("title", "hider"),
		insert("unlocks", "cone"),
		insert("unlocks", "barrel"),
		insert("unlocks", "cone"),
	}, "round-1")
	if err != nil || repeated {
		t.Fatal(err, repeated)
	}
	fields := next.GetRecord().GetFields()
	if fields["roundsWon"].GetNumber() != 2 || fields["score"].GetNumber() != -1.5 || fields["bestHide"].GetNumber() != 30 {
		t.Fatalf("numbers: %v", fields)
	}
	if fields["title"].GetText() != "hider" || strings.Join(fields["unlocks"].GetTags().GetMembers(), ",") != "barrel,cone" {
		t.Fatalf("text and tags: %v", fields)
	}
	if next.GetVersion() != 1 || len(next.GetRecentKeys()) != 1 {
		t.Fatalf("version %d keys %v", next.GetVersion(), next.GetRecentKeys())
	}

	// Each wrong operation is refused and the record is left as it was.
	wrong := map[string]*persistpb.Op{
		"cannot take max":    {Field: "roundsWon", Op: &persistpb.Op_Max{Max: 3}},
		"from zero up":       add("roundsWon", -1),
		"cannot take add":    add("bestHide", 1),
		"holds up to 8":      setText("title", "far too long"),
		"declares no field":  add("kills", 1),
		"cannot take insert": insert("title", "x"),
		"takes tags of 1":    insert("unlocks", ""),
		"names no operation": {Field: "score"},
	}
	for text, op := range wrong {
		_, _, err := Apply(stats, next, []*persistpb.Op{add("roundsWon", 1), op}, "")
		refused(t, err, text)
	}
	if next.GetRecord().GetFields()["roundsWon"].GetNumber() != 2 {
		t.Fatal("a refused update changed the record it was given")
	}
}

func TestApplyKeys(t *testing.T) {
	// A key seen among the recent keys is left out.
	next, _, err := Apply(stats, nil, []*persistpb.Op{add("roundsWon", 1)}, "round-1")
	if err != nil {
		t.Fatal(err)
	}
	again, repeated, err := Apply(stats, next, []*persistpb.Op{add("roundsWon", 1)}, "round-1")
	if err != nil || !repeated || again.GetRecord().GetFields()["roundsWon"].GetNumber() != 1 || again.GetVersion() != 1 {
		t.Fatalf("repeat: %v %v %v", again, repeated, err)
	}

	// Only the last RecentKeys are kept, so the oldest key counts again.
	for i := 2; i <= RecentKeys+1; i++ {
		if next, _, err = Apply(stats, next, []*persistpb.Op{add("roundsWon", 1)}, "round-"+strconv.Itoa(i)); err != nil {
			t.Fatal(err)
		}
	}
	if len(next.GetRecentKeys()) != RecentKeys || next.GetRecentKeys()[0] != "round-2" {
		t.Fatalf("keys: %d, first %s", len(next.GetRecentKeys()), next.GetRecentKeys()[0])
	}
	if _, repeated, _ := Apply(stats, next, []*persistpb.Op{add("roundsWon", 1)}, "round-1"); repeated {
		t.Fatal("a key past the recent ones is still a repeat")
	}
}

func TestApplyLimits(t *testing.T) {
	// Too many ops, a long key, too many tags and too large a record.
	ops := make([]*persistpb.Op, OpLimit+1)
	for i := range ops {
		ops[i] = add("roundsWon", 1)
	}
	_, _, err := Apply(stats, nil, ops, "")
	refused(t, err, "1 to 32 changes")
	_, _, err = Apply(stats, nil, ops[:0], "")
	refused(t, err, "1 to 32 changes")
	_, _, err = Apply(stats, nil, ops[:1], strings.Repeat("k", UpdateKeyBytes+1))
	refused(t, err, "key is up to 64 bytes")

	var next *persistpb.StoredRecord
	for i := range TagLimit {
		if next, _, err = Apply(stats, next, []*persistpb.Op{insert("unlocks", strconv.Itoa(i))}, ""); err != nil {
			t.Fatal(err)
		}
	}
	_, _, err = Apply(stats, next, []*persistpb.Op{insert("unlocks", "one-more")}, "")
	refused(t, err, "up to 256 tags")

	// Long tags fill a record past RecordBytes before the tag bound: 256
	// tags of 32 bytes take more than 8 KB, so the record's bound binds.
	next, err = nil, nil
	for i := 0; err == nil; i++ {
		if i == TagLimit {
			t.Fatal("256 long tags fit in one record")
		}
		tag := strings.Repeat("x", TagBytes-4) + strconv.Itoa(1000+i)
		var applied *persistpb.StoredRecord
		if applied, _, err = Apply(stats, next, []*persistpb.Op{insert("unlocks", tag)}, ""); err == nil {
			next = applied
		}
	}
	refused(t, err, "a persist record is up to 8192 bytes")
	if next.GetRecord().SizeVT() > RecordBytes {
		t.Fatal("a kept record passed the bound")
	}
}

func TestStoreSurvivesRestart(t *testing.T) {
	// A record written by one store reads back from a new one on the files.
	dir := t.TempDir()
	if _, _, err := newStore(dir).Update(76561197960265728, stats, []*persistpb.Op{add("roundsWon", 3)}, "round-1"); err != nil {
		t.Fatal(err)
	}
	restarted := newStore(dir)
	record, err := restarted.Get(76561197960265728, stats)
	if err != nil || record.GetFields()["roundsWon"].GetNumber() != 3 {
		t.Fatalf("after restart: %v %v", record, err)
	}

	// The recent keys survive too, so a resend after the restart counts once.
	_, repeated, err := restarted.Update(76561197960265728, stats, []*persistpb.Op{add("roundsWon", 3)}, "round-1")
	if err != nil || !repeated {
		t.Fatalf("resent after restart: %v %v", repeated, err)
	}

	// Another player's record is their own, and no temp file is left.
	record, err = restarted.Get(76561197960265729, stats)
	if err != nil || len(record.GetFields()) != 0 {
		t.Fatalf("other player: %v %v", record, err)
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) != 1 || entries[0].Name() != "76561197960265728.json" {
		t.Fatalf("files: %v", entries)
	}
}

func TestStoreRefusesLargeFile(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "1.json"), make([]byte, FileBytes+1), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := newStore(dir).Get(1, stats); err == nil || !strings.Contains(err.Error(), "past the") {
		t.Fatalf("want the bound, got %v", err)
	}
}

// TestTwoWritersBothLand is the conflict of two servers that update one
// player's record at once: every add lands, and a set's later write wins.
func TestTwoWritersBothLand(t *testing.T) {
	store := newStore(t.TempDir())
	const each = 50
	var wg sync.WaitGroup
	for server := range 2 {
		wg.Go(func() {
			for i := range each {
				key := "server-" + strconv.Itoa(server) + "-round-" + strconv.Itoa(i)
				if _, _, err := store.Update(7, stats, []*persistpb.Op{add("roundsWon", 1), insert("unlocks", key)}, key); err != nil {
					t.Error(err)
				}
			}
		})
	}
	wg.Wait()
	record, err := store.Get(7, stats)
	if err != nil {
		t.Fatal(err)
	}
	if got := record.GetFields()["roundsWon"].GetNumber(); got != 2*each {
		t.Fatalf("roundsWon %v, want %d", got, 2*each)
	}
	if got := len(record.GetFields()["unlocks"].GetTags().GetMembers()); got != 2*each {
		t.Fatalf("unlocks %d, want %d", got, 2*each)
	}
}

func TestAnswer(t *testing.T) {
	store := newStore(t.TempDir())
	m := manifest()
	players := func(slot int32) (uint64, bool) { return 100 + uint64(slot), slot < 4 }
	call := func(method string, request interface{ MarshalVT() ([]byte, error) }) ([]byte, error) {
		payload, _ := request.MarshalVT()
		return Answer(store, m, players, method, payload)
	}

	// An update and a read reach the player in the slot.
	data, err := call("Update", &persistpb.UpdateRequest{Player: 1, Type: "stats", Ops: []*persistpb.Op{add("roundsWon", 1)}, UpdateKey: "r1"})
	if err != nil {
		t.Fatal(err)
	}
	updated := &persistpb.UpdateReply{}
	if err := updated.UnmarshalVT(data); err != nil || updated.GetRecord().GetFields()["roundsWon"].GetNumber() != 1 {
		t.Fatalf("update: %v %v", updated, err)
	}
	data, err = call("GetMany", &persistpb.GetManyRequest{Players: []int32{1, 2}, Type: "stats"})
	if err != nil {
		t.Fatal(err)
	}
	many := &persistpb.GetManyReply{}
	if err := many.UnmarshalVT(data); err != nil || len(many.GetRecords()) != 2 || many.GetRecords()[0].GetRecord().GetFields()["roundsWon"].GetNumber() != 1 || len(many.GetRecords()[1].GetRecord().GetFields()) != 0 {
		t.Fatalf("getMany: %v %v", many, err)
	}

	// Each call outside the rules is refused.
	_, err = call("Get", &persistpb.GetRequest{Player: 1, Type: "settings"})
	refused(t, err, "declares no persist record type")
	_, err = call("Get", &persistpb.GetRequest{Player: 1, Type: "stats", Key: "alt"})
	refused(t, err, "take no key")
	_, err = call("Get", &persistpb.GetRequest{Player: 9, Type: "stats"})
	refused(t, err, "no player is in slot 9")
	_, err = call("GetMany", &persistpb.GetManyRequest{Players: make([]int32, ManyLimit+1), Type: "stats"})
	refused(t, err, "up to 16 players")
	_, err = Answer(store, m, players, "Get", []byte{0xff})
	refused(t, err, "malformed")
	_, err = Answer(store, m, players, "Delete", nil)
	refused(t, err, "no method Delete")
}
