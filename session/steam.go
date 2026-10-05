package session

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"

	"github.com/pkg/errors"
)

// DeadlockAppID is Deadlock's Steam application ID.
const DeadlockAppID = "1422450"

// libraryPath matches one library's path in libraryfolders.vdf.
var libraryPath = regexp.MustCompile(`"path"\s+"((?:[^"\\]|\\.)*)"`)

// loginUser matches one account's Steam ID and settings in loginusers.vdf.
var loginUser = regexp.MustCompile(`"(\d+)"\s*\{([^}]*)\}`)

// mostRecent matches the setting that marks the account signed in last.
var mostRecent = regexp.MustCompile(`"MostRecent"\s+"1"`)

// Steam is the local Steam installation: the client that launches games and
// the libraries they are installed in.
type Steam struct {
	// Root is the Steam installation directory.
	Root string
	// Libraries lists every library directory, Root first.
	Libraries []string
}

// FindSteam locates the Steam installation and reads its library folders.
func FindSteam() (*Steam, error) {
	// Take the first installation directory that has a library.
	var root string
	for _, candidate := range steamRoots() {
		if _, err := os.Stat(filepath.Join(candidate, "steamapps")); err == nil {
			root = candidate
			break
		}
	}
	if root == "" {
		return nil, errors.New("cannot find Steam; install Steam and Deadlock first")
	}

	// Add the libraries Steam lists, each once.
	steam := &Steam{Root: root, Libraries: []string{root}}
	folders, err := os.ReadFile(filepath.Join(root, "steamapps", "libraryfolders.vdf"))
	if err != nil {
		return steam, nil
	}
	for _, match := range libraryPath.FindAllSubmatch(folders, -1) {
		library := filepath.Clean(strings.ReplaceAll(string(match[1]), `\\`, `\`))
		if !sameDirectory(library, root) {
			steam.Libraries = append(steam.Libraries, library)
		}
	}
	return steam, nil
}

// App returns the installation directory of the app installed as name, such
// as "Deadlock", in the first library that has it.
func (s *Steam) App(name string) (string, bool) {
	for _, library := range s.Libraries {
		dir := filepath.Join(library, "steamapps", "common", name)
		if _, err := os.Stat(dir); err == nil {
			return dir, true
		}
	}
	return "", false
}

// Deadlock returns the Deadlock installation directory.
func (s *Steam) Deadlock() (string, error) {
	dir, ok := s.App("Deadlock")
	if !ok {
		return "", errors.New("cannot find Deadlock in the Steam libraries; set DEADLOCK_DIR")
	}
	return dir, nil
}

// Account returns the Steam ID of the account signed in to Steam last, or zero
// when Steam names none.
func (s *Steam) Account() uint64 {
	users, err := os.ReadFile(filepath.Join(s.Root, "config", "loginusers.vdf"))
	if err != nil {
		return 0
	}
	for _, match := range loginUser.FindAllSubmatch(users, -1) {
		if mostRecent.Match(match[2]) {
			id, _ := strconv.ParseUint(string(match[1]), 10, 64)
			return id
		}
	}
	return 0
}

// LaunchDeadlock starts Deadlock through the running Steam client with the
// given launch options, such as +connect. It returns once Steam took the
// request; the game keeps running on its own.
func (s *Steam) LaunchDeadlock(ctx context.Context, options ...string) error {
	arguments := append([]string{"-applaunch", DeadlockAppID}, options...)
	cmd := exec.CommandContext(ctx, steamExecutable(s.Root), arguments...)
	if err := cmd.Start(); err != nil {
		return errors.Wrap(err, "start Steam")
	}
	go func() { _ = cmd.Wait() }()
	return nil
}

// sameDirectory reports whether a and b name the same directory.
func sameDirectory(a, b string) bool {
	left, err := os.Stat(a)
	if err != nil {
		return false
	}
	right, err := os.Stat(b)
	return err == nil && os.SameFile(left, right)
}
