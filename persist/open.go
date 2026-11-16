package persist

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"

	"github.com/paralin/modlock/internal/filelock"
	"github.com/pkg/errors"
)

const (
	// LockFile is the exclusive process lock in a store directory.
	LockFile = "lock"
	// HolderFile names the process holding LockFile, without arguments.
	HolderFile = "holder"
)

// holderBytes bounds what a refused Open reads of HolderFile.
const holderBytes = 512

// storeSet retains the stores and lock files this process owns. Its lock
// serializes opens; per-store updates use Store.mu instead.
type storeSet struct {
	mu     sync.Mutex
	stores map[string]*Store
	locks  []*os.File
}

// processStores keeps each directory locked through mod reloads until exit.
var processStores = &storeSet{stores: make(map[string]*Store)}

// Open returns the store of the records kept in dir, creating dir, and holds
// dir's lock until this process exits. Within one process each directory has
// one store. It refuses a directory another process holds, naming that
// process, so two hosts never write one mod's records at once.
func Open(dir string) (*Store, error) {
	return processStores.open(dir)
}

// open returns the directory's one store and retains its process lock.
func (s *storeSet) open(dir string) (*Store, error) {
	// Return this process's store of dir if it has one.
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if store := s.stores[abs]; store != nil {
		return store, nil
	}

	// Lock the directory, or name the process that holds it.
	if err := makeDir(abs); err != nil {
		return nil, err
	}
	held, err := filelock.Lock(filepath.Join(abs, LockFile), true, false)
	if errors.Is(err, filelock.ErrLocked) {
		return nil, errors.Errorf("another modlock keeps these records in %s: %s; stop it, then start this one again", abs, holder(abs))
	}
	if err != nil {
		return nil, errors.Wrapf(err, "lock %s", abs)
	}

	// Name this process for the next one by its ID and program name, never
	// its arguments, which may carry a path or a token, and keep the lock for
	// its life. Only the next modlock of the same user reads the name.
	// A file an older modlock left keeps its mode through the write, so set
	// the mode too.
	name := fmt.Sprintf("process %d, %s", os.Getpid(), filepath.Base(os.Args[0]))
	path := filepath.Join(abs, HolderFile)
	if err := os.WriteFile(path, []byte(name), 0o600); err != nil {
		_ = held.Close()
		return nil, err
	}
	if err := os.Chmod(path, 0o600); err != nil {
		_ = held.Close()
		return nil, err
	}

	// Retain both the store and its exclusive lock for the process lifetime.
	store := &Store{dir: abs}
	s.stores[abs] = store
	s.locks = append(s.locks, held)
	return store, nil
}

// holder returns the name of the process holding dir's lock, as it wrote it.
func holder(dir string) string {
	// A refusal reads only the bounded process name left by the current holder.
	f, err := os.Open(filepath.Join(dir, HolderFile))
	if err != nil {
		return "a process that has not named itself"
	}
	defer f.Close()
	data, _ := io.ReadAll(io.LimitReader(f, holderBytes))
	if len(data) == 0 {
		return "a process that has not named itself"
	}
	return string(data)
}

// makeDir creates dir and its missing parents, and syncs the directory
// holding each one it created, so a crash cannot lose the directory under
// records the store already answered for.
func makeDir(dir string) error {
	// Note the directories that do not exist yet, innermost first.
	var made []string
	for d := dir; ; d = filepath.Dir(d) {
		if _, err := os.Stat(d); err == nil || filepath.Dir(d) == d {
			break
		}
		made = append(made, d)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	for _, d := range made {
		if err := syncDir(filepath.Dir(d)); err != nil {
			return err
		}
	}
	return nil
}
