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

	// Open the directory once, refusing a link, and do every file operation
	// through it, so neither a link nor a swap after the check redirects the
	// lock, the holder or the records.
	if err := notLink(abs); err != nil {
		return nil, err
	}
	if err := makeDir(abs); err != nil {
		return nil, err
	}
	root, err := openDir(abs)
	if err != nil {
		return nil, err
	}

	// Lock the directory, or name the process that holds it.
	held, err := openFile(root, abs, LockFile, os.O_RDWR|os.O_CREATE, 0o600)
	if err != nil {
		_ = root.Close()
		return nil, errors.Wrapf(err, "lock %s", abs)
	}
	if err := filelock.LockOpen(held, true, false); err != nil {
		_ = held.Close()
		if errors.Is(err, filelock.ErrLocked) {
			err = errors.Errorf("another modlock keeps these records in %s: %s; stop it, then start this one again", abs, holder(root, abs))
		} else {
			err = errors.Wrapf(err, "lock %s", abs)
		}
		_ = root.Close()
		return nil, err
	}

	// Name this process for the next one by its ID and program name, never
	// its arguments, which may carry a path or a token, and keep the lock for
	// its life. Only the next modlock of the same user reads the name.
	// A file an older modlock left keeps its mode through the write, so set
	// the mode too. The file is opened without truncating and emptied only
	// once it is known to be the store's own, so a refused link's target is
	// left as it was.
	name := fmt.Sprintf("process %d, %s", os.Getpid(), filepath.Base(os.Args[0]))
	if err := writeHolder(root, abs, name); err != nil {
		_ = held.Close()
		_ = root.Close()
		return nil, err
	}

	// Retain the store, its directory and its exclusive lock for the process
	// lifetime.
	store := &Store{dir: abs, root: root}
	s.stores[abs] = store
	s.locks = append(s.locks, held)
	return store, nil
}

// writeHolder replaces the contents of dir's HolderFile with name.
func writeHolder(root *os.Root, dir, name string) error {
	// Open the file only if it is the directory's own, then replace its
	// contents and mode.
	f, err := openFile(root, dir, HolderFile, os.O_WRONLY|os.O_CREATE, 0o600)
	if err != nil {
		return err
	}
	if err := f.Truncate(0); err != nil {
		_ = f.Close()
		return err
	}
	if _, err := f.WriteString(name); err != nil {
		_ = f.Close()
		return err
	}
	if err := f.Chmod(0o600); err != nil {
		_ = f.Close()
		return err
	}
	return f.Close()
}

// holder returns the name of the process holding dir's lock, as it wrote it.
func holder(root *os.Root, dir string) string {
	// A refusal reads only the bounded process name left by the current holder.
	f, err := openFile(root, dir, HolderFile, os.O_RDONLY, 0)
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
		if err := syncDir(filepath.Dir(d), nil); err != nil {
			return err
		}
	}
	return nil
}

// openDir opens dir as the root of every file a store opens. It refuses a
// link at dir, and checks once the directory is open that it is the one at
// dir, so a link swapped in between the check and the open is refused too.
func openDir(dir string) (*os.Root, error) {
	// Open the directory, then check it is still the one named dir.
	root, err := os.OpenRoot(dir)
	if err != nil {
		return nil, err
	}
	opened, err := root.Stat(".")
	if err == nil {
		var info os.FileInfo
		if info, err = os.Lstat(dir); err == nil && (!info.IsDir() || !os.SameFile(opened, info)) {
			err = linked(dir)
		}
	}
	if err != nil {
		_ = root.Close()
		return nil, err
	}
	return root, nil
}

// openFile opens name in the store directory root, which is at dir, and
// refuses it unless it is a plain file of the directory's own. The root keeps
// the open inside the directory, but it follows a link that leads to another
// file there, so the file is checked before the open and again after it: a
// link swapped in between is refused before anything is read or written. The
// flag must not truncate, so a refused file is left as it was.
func openFile(root *os.Root, dir, name string, flag int, perm os.FileMode) (*os.File, error) {
	// Refuse a link or any other entry that is not a plain file.
	path := filepath.Join(dir, name)
	if info, err := root.Lstat(name); err == nil && !info.Mode().IsRegular() {
		return nil, linked(path)
	}
	f, err := root.OpenFile(name, flag, perm)
	if err != nil {
		return nil, err
	}

	// The open file must be the plain file the directory names.
	opened, err := f.Stat()
	if err == nil {
		var info os.FileInfo
		if info, err = root.Lstat(name); err == nil && (!info.Mode().IsRegular() || !os.SameFile(opened, info)) {
			err = linked(path)
		}
	}
	if err != nil {
		_ = f.Close()
		return nil, err
	}
	return f, nil
}

// notLink refuses a link at path; a path that does not exist passes.
func notLink(path string) error {
	// A missing path is made later as a directory of its own.
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return linked(path)
	}
	return nil
}

// linked is the error of a store path that is a link or not a plain entry
// of the store's own.
func linked(path string) error {
	return errors.Errorf("persist: %s is a link or not a plain file or directory; Modlock keeps no records through one, so remove it", path)
}
