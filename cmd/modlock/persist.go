package main

import (
	"context"
	"os"
	"path/filepath"
	"sync"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/persist"
	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
	"github.com/pkg/errors"
)

// persistDir is the directory a local session keeps the records of the mod
// slug built at path in: the project's own when the build is a project's,
// otherwise one in Modlock's configuration directory. Empty keeps them for
// the session only. A built mod's manifest is not checked as a project's is,
// so the slug is checked here before it names any directory, and the
// directory must stay beneath the one Modlock keeps records in.
func persistDir(path, slug string) (string, error) {
	// Refuse a slug that cannot name a directory, then keep a project's
	// records in the project.
	if err := check.Slug(slug); err != nil {
		return "", errors.Wrap(err, "persist")
	}
	if fixtures := projectFixtures(path); fixtures != "" {
		// A checkout can carry .modlock or .modlock/persist as a committed
		// link, which would take the records, the lock and the holder file
		// wherever it leads.
		dir := filepath.Join(filepath.Dir(fixtures), persist.Dir)
		for _, d := range []string{filepath.Dir(dir), dir} {
			if err := ownDir(d, slug); err != nil {
				return "", err
			}
		}
		return dir, nil
	}

	// Keep a built mod's records beneath Modlock's persist directory.
	root := configPath("persist")
	if root == "" {
		return "", nil
	}
	dir := filepath.Join(root, slug)
	if rel, err := filepath.Rel(root, dir); err != nil || rel != slug {
		return "", errors.Errorf("persist: the records of %q would leave %s", slug, root)
	}

	// The name stays inside only if nothing there leads elsewhere: a link
	// already at dir would pass the check above and take the records out.
	if err := ownDir(dir, slug); err != nil {
		return "", err
	}
	return dir, nil
}

// ownDir refuses dir, a directory the records of slug are kept in or
// beneath, when it is a link or not a directory. A dir not made yet passes.
func ownDir(dir, slug string) error {
	info, err := os.Lstat(dir)
	if err == nil && (info.Mode()&os.ModeSymlink != 0 || !info.IsDir()) {
		return errors.Errorf("persist: %s is not a directory of its own; remove it to keep the records of %q there", dir, slug)
	}
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return errors.Wrap(err, "persist")
	}
	return nil
}

// localPersist answers the persist calls of a local server's mods from files
// on this computer, so a mod's records survive a restart of the session. It
// learns which player is in each slot from the host's events. Persist is
// always answered here, even when the session forwards other services: the
// host names only a slot, so a forwarded call could not say whose record it
// reaches.
type localPersist struct {
	// mods are the mods that declare persist record types, by slug.
	mods map[string]*persistMod
	// mu guards slots.
	mu sync.Mutex
	// slots are the players in the server, by slot.
	slots map[int32]slotPlayer
}

// slotPlayer is the player the host says holds a slot, and the generation of
// their connection, zero from a host that does not report it.
type slotPlayer struct {
	steamID    uint64
	generation uint32
}

// persistMod is one mod's declared record types and the store of its records.
type persistMod struct {
	manifest *wasm.Manifest
	store    *persist.Store
}

// newLocalPersist returns the local persist service of the mods built at
// dirs, printing where each mod's records are kept.
func newLocalPersist(out *printer, dirs []string) (*localPersist, error) {
	local := &localPersist{mods: map[string]*persistMod{}, slots: map[int32]slotPlayer{}}
	for _, dir := range dirs {
		manifest, err := project.ReadManifest(dir)
		if err != nil {
			return nil, err
		}
		if len(manifest.GetPersist()) == 0 {
			continue
		}
		slug := manifest.GetSlug()
		records, err := persistDir(dir, slug)
		if err != nil {
			return nil, err
		}
		if records == "" {
			if records, err = os.MkdirTemp("", "modlock-persist-"); err != nil {
				return nil, err
			}
		}
		store, err := persist.Open(records)
		if err != nil {
			return nil, err
		}
		local.mods[slug] = &persistMod{manifest: manifest, store: store}
		out.mark("·", dim, slug+": service persist: kept in "+records)
	}
	return local, nil
}

// declares reports whether the mod slug keeps records here.
func (l *localPersist) declares(slug string) bool {
	return l.mods[slug] != nil
}

// follow notes the players who join and leave, then passes event to events.
func (l *localPersist) follow(events func(*control.HostEvent)) func(*control.HostEvent) {
	return func(event *control.HostEvent) {
		if joined := event.GetJoined(); joined != nil {
			l.mu.Lock()
			l.slots[joined.GetSlot()] = slotPlayer{steamID: joined.GetSteamId(), generation: joined.GetGeneration()}
			l.mu.Unlock()
		}
		if left := event.GetLeft(); left != nil {
			l.mu.Lock()
			delete(l.slots, left.GetSlot())
			l.mu.Unlock()
		}
		events(event)
	}
}

// player returns the SteamID64 of the player in slot and the generation of
// their connection, as the host's join event named them; a bot has none. A
// host too old to name the generation reports zero, which Answer refuses.
func (l *localPersist) player(slot int32) (uint64, uint32, bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	held := l.slots[slot]
	return held.steamID, held.generation, held.steamID != 0
}

// answer answers the persist calls of the mods that keep records here and
// passes every other forwarded call to next, which may be nil.
func (l *localPersist) answer(next func(context.Context, *control.ServiceCall) ([]byte, error)) func(context.Context, *control.ServiceCall) ([]byte, error) {
	return func(ctx context.Context, call *control.ServiceCall) ([]byte, error) {
		if mod := l.mods[call.GetMod()]; mod != nil && call.GetService() == persist.Service {
			return persist.Answer(mod.store, mod.manifest, l.player, call.GetMethod(), call.GetPayload())
		}
		if next == nil {
			return nil, errors.New("this session forwards no services")
		}
		return next(ctx, call)
	}
}
