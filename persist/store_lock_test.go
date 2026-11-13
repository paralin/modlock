package persist

import (
	"bufio"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"

	persistpb "github.com/paralin/modlock/src/wasm/persist"
)

// holdEnv names the directory a helper process opens and holds.
const holdEnv = "MODLOCK_PERSIST_TEST_HOLD"

// TestHoldStoreHelper is the first host when holdEnv is set: it opens the
// store, says so, and holds it until its input closes.
func TestHoldStoreHelper(t *testing.T) {
	dir := os.Getenv(holdEnv)
	if dir == "" {
		t.Skip("run by TestSecondHostRefused")
	}
	if _, err := Open(dir); err != nil {
		t.Fatal(err)
	}
	os.Stdout.WriteString("held\n")
	_, _ = bufio.NewReader(os.Stdin).ReadString('\n')
}

// TestSecondHostRefused starts a first host in another process and checks
// that a second cannot open the same records, that its error names the
// first, and that the records open once the first exits.
func TestSecondHostRefused(t *testing.T) {
	// Start the first host and wait until it holds the store.
	dir := t.TempDir()
	first := exec.Command(os.Args[0], "-test.run=^TestHoldStoreHelper$", "-test.count=1")
	first.Env = append(os.Environ(), holdEnv+"="+dir)
	stdin, err := first.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
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
	// process ID and its command line.
	_, err = Open(dir)
	pid := "process " + strconv.Itoa(first.Process.Pid)
	if err == nil || !strings.Contains(err.Error(), "another modlock keeps these records") || !strings.Contains(err.Error(), pid) || !strings.Contains(err.Error(), "-test.run=^TestHoldStoreHelper$") {
		t.Fatalf("second host: got %v, want a refusal naming %s and its arguments", err, pid)
	}

	// Once the first exits, the records open and take an update.
	_ = stdin.Close()
	if err := first.Wait(); err != nil {
		t.Fatalf("first host exit: %v", err)
	}
	store, err := Open(dir)
	if err != nil {
		t.Fatalf("after the first exits: %v", err)
	}
	if _, _, err := store.Update(76561197960265728, stats, []*persistpb.Op{add("roundsWon", 1)}, "round-1"); err != nil {
		t.Fatal(err)
	}

	// Within one process, opening the records again returns the same store.
	again, err := Open(dir)
	if err != nil || again != store {
		t.Fatalf("second open in one process: %p %v, want %p", again, err, store)
	}
}
