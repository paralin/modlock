package persist

import (
	"bufio"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// holdEnv names the directory a helper process opens and holds.
const holdEnv = "MODLOCK_PERSIST_TEST_HOLD"

// TestHoldStoreHelper is the first host when holdEnv is set: it writes a
// record, says so, and holds the store until its input closes.
func TestHoldStoreHelper(t *testing.T) {
	// Only the subprocess owns this directory while it writes the test record.
	dir := os.Getenv(holdEnv)
	if dir == "" {
		t.Skip("run by TestSecondHostRefused")
	}
	store, err := Open(dir)
	if err != nil {
		t.Fatal(err)
	}
	ops := []*persistpb.Op{
		add("roundsWon", 3),
		{Field: "bestHide", Op: &persistpb.Op_Max{Max: 41}},
		setText("title", "winner"),
		insert("unlocks", "crate"),
	}
	if _, _, err := store.Update(76561197960265728, stats, ops, "round-1"); err != nil {
		t.Fatal(err)
	}

	// Keep the writer alive until its caller has checked the exclusive lock.
	os.Stdout.WriteString("held\n")
	_, _ = bufio.NewReader(os.Stdin).ReadString('\n')
}

// TestSecondHostRefused starts a first host in another process and checks
// that a second cannot open the same records, that its error names the
// first by process ID and program name only, that the holder file is the
// owner's alone, and that the record and its update key survive that
// process's exit and a second process opening the store.
func TestSecondHostRefused(t *testing.T) {
	// Start the first host and wait until it holds the store.
	// An older modlock left a world-readable holder file with its arguments.
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, HolderFile), []byte("process 1, modlock play -test.run=old"), 0o644); err != nil {
		t.Fatal(err)
	}
	first := exec.Command(os.Args[0], "-test.run=^TestHoldStoreHelper$", "-test.count=1")
	first.Env = append(os.Environ(), holdEnv+"="+dir)
	stdin, err := first.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}

	// Wait on the writer's readiness message before checking exclusive admission.
	stdout, err := first.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := first.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = first.Process.Kill(); _ = first.Wait() }()
	line, err := bufio.NewReader(stdout).ReadString('\n')
	if err != nil || line != "held\n" {
		t.Fatalf("first host: %q %v", line, err)
	}

	// The second is refused and told which process holds the records, by its
	// process ID and program name, never its arguments, and the holder file
	// is the owner's alone.
	_, err = Open(dir)
	pid := "process " + strconv.Itoa(first.Process.Pid)
	program := filepath.Base(os.Args[0])
	if err == nil || !strings.Contains(err.Error(), "another modlock keeps these records") || !strings.Contains(err.Error(), pid+", "+program) || strings.Contains(err.Error(), "-test.run") {
		t.Fatalf("second host: got %v, want a refusal naming %s and %s without arguments", err, pid, program)
	}
	held, err := os.ReadFile(filepath.Join(dir, HolderFile))
	if err != nil || string(held) != pid+", "+program {
		t.Fatalf("holder file: %q %v, want %q", held, err, pid+", "+program)
	}
	if info, err := os.Stat(filepath.Join(dir, HolderFile)); err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("holder file mode: %v %v, want 0600", info.Mode().Perm(), err)
	}

	// Once the first exits, a new process reads every field it kept.
	_ = stdin.Close()
	if err := first.Wait(); err != nil {
		t.Fatalf("first host exit: %v", err)
	}
	store, err := Open(dir)
	if err != nil {
		t.Fatalf("after the first exits: %v", err)
	}
	record, err := store.Get(76561197960265728, stats)
	if err != nil {
		t.Fatal(err)
	}
	fields := record.GetFields()
	if fields["roundsWon"].GetNumber() != 3 || fields["bestHide"].GetNumber() != 41 || fields["title"].GetText() != "winner" || strings.Join(fields["unlocks"].GetTags().GetMembers(), ",") != "crate" {
		t.Fatalf("after the writer process exits: %v", record)
	}

	// Resending the first process's update after restart counts it once.
	_, repeated, err := store.Update(76561197960265728, stats, []*persistpb.Op{add("roundsWon", 3)}, "round-1")
	if err != nil || !repeated {
		t.Fatalf("resent after process restart: repeated %v, error %v", repeated, err)
	}
	record, err = store.Get(76561197960265728, stats)
	if err != nil || record.GetFields()["roundsWon"].GetNumber() != 3 {
		t.Fatalf("resent record: %v, error %v", record, err)
	}

	// A new round can still change the reopened record.
	if _, _, err := store.Update(76561197960265728, stats, []*persistpb.Op{add("roundsWon", 1)}, "round-2"); err != nil {
		t.Fatal(err)
	}

	// Within one process, opening the records again returns the same store.
	again, err := Open(dir)
	if err != nil || again != store {
		t.Fatalf("second open in one process: %p %v, want %p", again, err, store)
	}
}
