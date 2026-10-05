package session

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"

	"github.com/pkg/errors"
)

// Proton is one Steam Proton installation with the prefix modlock-host runs
// in. The prefix is the command line's own: the host must not share the
// game's prefix, because Steam runs the client under its own Proton, whose
// wineserver would wait for this one to exit.
type Proton struct {
	// Dir is the Proton installation directory.
	Dir string
	// Prefix is the compatibility data directory of the host's prefix.
	Prefix string
	// steam is the Steam installation Proton runs under.
	steam *Steam
}

// FindProton returns MODLOCK_PROTON when set, else the newest numbered Proton
// in the Steam libraries, else Proton Experimental.
func FindProton(steam *Steam) (*Proton, error) {
	// Place the prefix in the user cache.
	cache, err := os.UserCacheDir()
	if err != nil {
		return nil, errors.Wrap(err, "locate the cache directory")
	}
	proton := &Proton{Prefix: filepath.Join(cache, "modlock", "proton"), steam: steam}

	// Take the configured Proton.
	if dir := os.Getenv("MODLOCK_PROTON"); dir != "" {
		proton.Dir = dir
		return proton, nil
	}

	// Rank the installed Protons by version.
	type candidate struct {
		dir     string
		version []int
	}
	var candidates []candidate
	for _, library := range steam.Libraries {
		matches, _ := filepath.Glob(filepath.Join(library, "steamapps", "common", "Proton *", "proton"))
		for _, match := range matches {
			name := strings.TrimPrefix(filepath.Base(filepath.Dir(match)), "Proton ")
			version, ok := protonVersion(name)
			if !ok && name != "- Experimental" {
				continue
			}
			candidates = append(candidates, candidate{dir: filepath.Dir(match), version: version})
		}
	}
	if len(candidates) == 0 {
		return nil, errors.New("cannot find Proton; install Proton in Steam or set MODLOCK_PROTON")
	}
	// Experimental has no version, so any numbered release outranks it.
	best := slices.MaxFunc(candidates, func(a, b candidate) int {
		return slices.Compare(a.version, b.version)
	})
	proton.Dir = best.dir
	return proton, nil
}

// protonVersion parses a release name such as "11.0", ignoring a suffix such
// as " (Beta)".
func protonVersion(name string) ([]int, bool) {
	name, _, _ = strings.Cut(name, " ")
	var version []int
	for part := range strings.SplitSeq(name, ".") {
		number, err := strconv.Atoi(part)
		if err != nil {
			return nil, false
		}
		version = append(version, number)
	}
	return version, true
}

// Prepare creates the prefix when it does not exist yet. Proton sets a prefix
// up for every verb except runinprefix; getcompatpath does so and exits.
func (p *Proton) Prepare(ctx context.Context) error {
	if _, err := os.Stat(filepath.Join(p.Prefix, "pfx")); err == nil {
		return nil
	}
	if err := os.MkdirAll(p.Prefix, 0o755); err != nil {
		return errors.Wrap(err, "create the Proton prefix")
	}
	cmd := p.script(ctx, "getcompatpath", "/")
	if output, err := cmd.CombinedOutput(); err != nil {
		return errors.Wrapf(err, "create the Proton prefix: %s", output)
	}
	return nil
}

// Command returns a command that runs the Windows program at path in the
// prefix. runinprefix starts it directly; run would wrap it in Proton's
// steam.exe, which leaves its console output silent.
func (p *Proton) Command(ctx context.Context, path string, arguments ...string) *exec.Cmd {
	return p.script(ctx, append([]string{"runinprefix", path}, arguments...)...)
}

// Kill ends every process in the prefix.
func (p *Proton) Kill() {
	cmd := exec.Command(filepath.Join(p.Dir, "files", "bin", "wineserver"), "-k")
	cmd.Env = append(os.Environ(), "WINEPREFIX="+filepath.Join(p.Prefix, "pfx"))
	_ = cmd.Run()
}

// script returns a command that runs the proton script with arguments.
func (p *Proton) script(ctx context.Context, arguments ...string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, filepath.Join(p.Dir, "proton"), arguments...)
	cmd.Env = append(os.Environ(),
		"STEAM_COMPAT_CLIENT_INSTALL_PATH="+p.steam.Root,
		"STEAM_COMPAT_DATA_PATH="+p.Prefix,
		"STEAM_COMPAT_APP_ID="+DeadlockAppID,
		"SteamAppId="+DeadlockAppID,
		"SteamGameId="+DeadlockAppID,
		"WINEDEBUG=-all",
	)
	return cmd
}

// WindowsPath names an absolute Linux path as Wine's Z: drive sees it.
func WindowsPath(path string) string {
	return `Z:` + strings.ReplaceAll(path, "/", `\`)
}
