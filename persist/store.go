package persist

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"

	"github.com/paralin/modlock/internal/filelock"
	"github.com/paralin/modlock/src/wasm"
	persistpb "github.com/paralin/modlock/src/wasm/persist"
	"github.com/pkg/errors"
)

// Dir is the directory of a mod project that a local host keeps the mod's
// records in, one file per player.
const Dir = ".modlock/persist"

// FileBytes bounds one player's file: every type's record at its bound with
// its recent keys, and room for the names around them.
const FileBytes = TypeLimit*(RecordBytes+RecentKeys*(UpdateKeyBytes+2)) + 16<<10

// LockFile is the file in a store's directory that the one process keeping
// its records holds locked, and HolderFile names that process, so a second
// host started on the same records is refused with the first one's name.
const (
	LockFile   = "lock"
	HolderFile = "holder"
)

// holderBytes bounds what a refused Open reads of HolderFile.
const holderBytes = 512

// Store keeps one mod's records in a directory, one file per player, so
// they survive a restart of the host. Each update reads the player's file,
// applies its ops and replaces the file whole, one update at a time, so two
// updates of one record at once both land.
type Store struct {
	// dir holds the files.
	dir string
	// mu orders the updates, so each reads the file the last one wrote.
	mu sync.Mutex
}

// opened are the stores this process holds the lock of, by directory, and
// their lock files. A store stays open, and its directory locked, until the
// process exits, so a rebuilt mod keeps writing through the store its first
// build opened.
var opened = struct {
	sync.Mutex
	stores map[string]*Store
	locks  []*os.File
}{stores: map[string]*Store{}}

// Open returns the store of the records kept in dir, creating dir, and holds
// dir's lock until this process exits. Within one process each directory has
// one store. It refuses a directory another process holds, naming that
// process, so two hosts never write one mod's records at once.
func Open(dir string) (*Store, error) {
	// Return this process's store of dir if it has one.
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, err
	}
	opened.Lock()
	defer opened.Unlock()
	if store := opened.stores[abs]; store != nil {
		return store, nil
	}

	// Lock the directory, or name the process that holds it.
	if err := os.MkdirAll(abs, 0o755); err != nil {
		return nil, err
	}
	held, err := filelock.Lock(filepath.Join(abs, LockFile), true, false)
	if errors.Is(err, filelock.ErrLocked) {
		return nil, errors.Errorf("another modlock keeps these records in %s: %s; stop it, then start this one again", abs, holder(abs))
	}
	if err != nil {
		return nil, errors.Wrapf(err, "lock %s", abs)
	}

	// Name this process for the next one, and keep the lock for its life.
	name := fmt.Sprintf("process %d, %s", os.Getpid(), strings.Join(os.Args, " "))
	if err := os.WriteFile(filepath.Join(abs, HolderFile), []byte(name), 0o644); err != nil {
		_ = held.Close()
		return nil, err
	}
	store := &Store{dir: abs}
	opened.stores[abs] = store
	opened.locks = append(opened.locks, held)
	return store, nil
}

// holder returns the name of the process holding dir's lock, as it wrote it.
func holder(dir string) string {
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

// newStore returns a store of the records kept in dir without its lock, for
// tests that stand in for one host.
func newStore(dir string) *Store {
	return &Store{dir: dir}
}

// Dir returns the directory the store keeps its files in.
func (s *Store) Dir() string {
	return s.dir
}

// path is the file of the player steamID.
func (s *Store) path(steamID uint64) string {
	return filepath.Join(s.dir, strconv.FormatUint(steamID, 10)+".json")
}

// read returns the file of the player steamID, empty when none is kept. A
// file past FileBytes is refused rather than read.
func (s *Store) read(steamID uint64) (*persistpb.PlayerFile, error) {
	// Read the file within its bound.
	path := s.path(steamID)
	info, err := os.Stat(path)
	if errors.Is(err, os.ErrNotExist) {
		return &persistpb.PlayerFile{SteamId: steamID}, nil
	}
	if err != nil {
		return nil, err
	}
	if info.Size() > FileBytes {
		return nil, errors.Errorf("%s is %d bytes, past the %d a player's records may take", path, info.Size(), FileBytes)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	// Parse it as the store wrote it.
	file := &persistpb.PlayerFile{}
	if err := file.UnmarshalJSON(data); err != nil {
		return nil, errors.Wrapf(err, "parse %s", path)
	}
	file.SteamId = steamID
	return file, nil
}

// Get returns the player steamID's record of recordType, empty for one never
// written.
func (s *Store) Get(steamID uint64, recordType *persistpb.RecordType) (*persistpb.Record, error) {
	file, err := s.read(steamID)
	if err != nil {
		return nil, err
	}
	return recordOf(file.GetRecords()[recordType.GetName()]), nil
}

// Update applies ops to the player steamID's record of recordType, as Apply
// does, and keeps the result. It returns the record after the update and
// whether the update's key made it a repeat.
func (s *Store) Update(steamID uint64, recordType *persistpb.RecordType, ops []*persistpb.Op, updateKey string) (*persistpb.Record, bool, error) {
	// Read the player's file and apply the update to its record.
	s.mu.Lock()
	defer s.mu.Unlock()
	file, err := s.read(steamID)
	if err != nil {
		return nil, false, err
	}
	name := recordType.GetName()
	next, repeated, err := Apply(recordType, file.GetRecords()[name], ops, updateKey)
	if err != nil || repeated {
		return recordOf(next), repeated, err
	}

	// Replace the file whole, so a crash leaves the old file or the new.
	if file.Records == nil {
		file.Records = map[string]*persistpb.StoredRecord{}
	}
	file.Records[name] = next
	data, err := file.MarshalJSON()
	if err != nil {
		return nil, false, err
	}
	return recordOf(next), false, s.write(s.path(steamID), data)
}

// write replaces path with data through a file beside it.
func (s *Store) write(path string, data []byte) error {
	if err := os.MkdirAll(s.dir, 0o755); err != nil {
		return err
	}
	temp, err := os.CreateTemp(s.dir, ".update-*")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name())
	if _, err := temp.Write(data); err != nil {
		_ = temp.Close()
		return err
	}
	if err := temp.Sync(); err != nil {
		_ = temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	return os.Rename(temp.Name(), path)
}

// recordOf returns stored's record, or an empty one.
func recordOf(stored *persistpb.StoredRecord) *persistpb.Record {
	if record := stored.GetRecord(); record != nil {
		return record
	}
	return &persistpb.Record{}
}

// Players resolves a server slot to the SteamID64 of the player in it, and
// reports false for an empty slot.
type Players func(slot int32) (uint64, bool)

// Answer answers one call of the persist service from the mod manifest
// describes: its record types, its players and the store of its records. It
// returns the reply's bytes, or a Refusal for a call that breaks a rule or a
// bound.
func Answer(store *Store, manifest *wasm.Manifest, players Players, method string, payload []byte) ([]byte, error) {
	// Find the record type and player a call reaches.
	reach := func(typeName, key string, slot int32) (*persistpb.RecordType, uint64, error) {
		recordType := Find(manifest, typeName)
		if recordType == nil {
			return nil, 0, refuse("mod.json declares no persist record type %q", typeName)
		}
		if err := CheckKey(key); err != nil {
			return nil, 0, err
		}
		steamID, ok := players(slot)
		if !ok {
			return nil, 0, refuse("no player is in slot %d", slot)
		}
		return recordType, steamID, nil
	}

	switch method {
	case "Get":
		request := &persistpb.GetRequest{}
		if err := request.UnmarshalVT(payload); err != nil {
			return nil, refuse("the persist Get request is malformed")
		}
		recordType, steamID, err := reach(request.GetType(), request.GetKey(), request.GetPlayer())
		if err != nil {
			return nil, err
		}
		record, err := store.Get(steamID, recordType)
		if err != nil {
			return nil, err
		}
		return (&persistpb.GetReply{Record: record}).MarshalVT()
	case "GetMany":
		request := &persistpb.GetManyRequest{}
		if err := request.UnmarshalVT(payload); err != nil {
			return nil, refuse("the persist GetMany request is malformed")
		}
		if len(request.GetPlayers()) > ManyLimit {
			return nil, refuse("a persist GetMany reads up to %d players", ManyLimit)
		}
		reply := &persistpb.GetManyReply{}
		for _, slot := range request.GetPlayers() {
			recordType, steamID, err := reach(request.GetType(), "", slot)
			if err != nil {
				return nil, err
			}
			record, err := store.Get(steamID, recordType)
			if err != nil {
				return nil, err
			}
			reply.Records = append(reply.Records, &persistpb.PlayerRecord{Player: slot, Record: record})
		}
		return reply.MarshalVT()
	case "Update":
		request := &persistpb.UpdateRequest{}
		if err := request.UnmarshalVT(payload); err != nil {
			return nil, refuse("the persist Update request is malformed")
		}
		recordType, steamID, err := reach(request.GetType(), request.GetKey(), request.GetPlayer())
		if err != nil {
			return nil, err
		}
		record, repeated, err := store.Update(steamID, recordType, request.GetOps(), request.GetUpdateKey())
		if err != nil {
			return nil, err
		}
		return (&persistpb.UpdateReply{Record: record, Repeated: repeated}).MarshalVT()
	}
	return nil, refuse("the persist service has no method %s", method)
}
