package persist

import (
	"os"
	"path/filepath"
	"strconv"
	"sync"

	"github.com/paralin/modlock/src/wasm"
	persistpb "github.com/paralin/modlock/src/wasm/persist"
	"github.com/pkg/errors"
)

// Dir is the directory of a mod project that a local host keeps the mod's
// records in, one file per player.
const Dir = ".modlock/persist"

// FileBytes bounds one player's JSON file: every type's binary record bound,
// its recent keys and envelope, allowing six JSON bytes for each source byte
// when strings and keys contain control characters.
const FileBytes = 6 * (TypeLimit*(RecordBytes+RecentKeys*(UpdateKeyBytes+2)) + 16<<10)

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

// write replaces path with data through a file beside it, and returns once
// both the data and the replace would survive an OS crash or power loss.
func (s *Store) write(path string, data []byte) error {
	// Never acknowledge a file the reader's bound would make unreadable.
	if len(data) > FileBytes {
		return refuse("a player's encoded persist records are up to %d bytes", FileBytes)
	}

	// Replace in the same directory so readers see the old or new whole file.
	if err := makeDir(s.dir); err != nil {
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

	// Answer only once the new name is durable too. A failure past the
	// rename may leave the update kept, as a lost answer does; a resend with
	// the same update key counts once.
	return replace(temp.Name(), path)
}

// recordOf returns stored's record, or an empty one.
func recordOf(stored *persistpb.StoredRecord) *persistpb.Record {
	if record := stored.GetRecord(); record != nil {
		return record
	}
	return &persistpb.Record{}
}

// Players resolves a server slot to the SteamID64 of the player in it and
// the generation of their connection, and reports false for an empty slot.
// A host that does not learn generations reports zero, which leaves the
// check of a request's generation to the mod library.
type Players func(slot int32) (steamID uint64, generation uint32, ok bool)

// Answer answers one call of the persist service from the mod manifest
// describes: its record types, its players and the store of its records. It
// returns the reply's bytes, or a Refusal for a call that breaks a rule or a
// bound. A call reaches a player's record only when it names the generation
// of the connection in the slot, so a Player a mod kept after its player left
// cannot reach the next player in the slot.
func Answer(store *Store, manifest *wasm.Manifest, players Players, method string, payload []byte) ([]byte, error) {
	// Find the record type and player a call reaches.
	reach := func(typeName, key string, slot int32, generation uint32) (*persistpb.RecordType, uint64, error) {
		// A slot reaches only the declared record of the human who holds it,
		// on the connection the call was made for.
		recordType := Find(manifest, typeName)
		if recordType == nil {
			return nil, 0, refuse("mod.json declares no persist record type %q", typeName)
		}
		if err := CheckKey(key); err != nil {
			return nil, 0, err
		}
		if generation == 0 {
			return nil, 0, refuse("a persist request names its player's connection generation; build the mod with this Modlock")
		}
		steamID, held, ok := players(slot)
		if !ok {
			return nil, 0, refuse("no player is in slot %d", slot)
		}
		if held != 0 && held != generation {
			return nil, 0, refuse("the player the request names left slot %d", slot)
		}
		return recordType, steamID, nil
	}

	switch method {
	case "Get":
		request := &persistpb.GetRequest{}
		if err := request.UnmarshalVT(payload); err != nil {
			return nil, refuse("the persist Get request is malformed")
		}
		recordType, steamID, err := reach(request.GetType(), request.GetKey(), request.GetPlayer(), request.GetGeneration())
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
		if len(request.GetGenerations()) != len(request.GetPlayers()) {
			return nil, refuse("a persist GetMany names one connection generation for each player")
		}
		reply := &persistpb.GetManyReply{}
		for i, slot := range request.GetPlayers() {
			recordType, steamID, err := reach(request.GetType(), "", slot, request.GetGenerations()[i])
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
		recordType, steamID, err := reach(request.GetType(), request.GetKey(), request.GetPlayer(), request.GetGeneration())
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
