package publish

import (
	"archive/tar"
	"compress/gzip"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"

	"github.com/paralin/modlock/src/wasm"
)

// TestPublish uploads a mod, its map and its client content with a saved session and reads back
// the service's answer and its refusals.
func TestPublish(t *testing.T) {
	// Lay out a built mod with a map, client content and a log that must
	// stay out.
	dir := t.TempDir()
	for _, sub := range []string{"maps", "panorama/layout/hello-1.0.0"} {
		if err := os.MkdirAll(filepath.Join(dir, sub), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	for name, content := range map[string]string{
		"mod.json": "{}", "mod.wasm": "module", "server.log": "log", "maps/hl_court.vpk": "map",
		"panorama/layout/hello-1.0.0/panel.vxml_c": "layout",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	mod := &Mod{Dir: dir, Manifest: &wasm.Manifest{Slug: "hello", Version: "1.0.0", Entry: "mod.wasm", Map: "hl_court"}, Notes: "Faster rounds"}

	// Serve the release endpoint, recording what each upload carried.
	var uploaded []string
	var notes, prerelease string
	refuse := false
	service := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Refuse any other path or session.
		if r.URL.Path != releasesPath || r.Header.Get("Authorization") != "Bearer saved" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}

		// Record the upload, then answer with a refusal or the release.
		uploaded = tarNames(t, r.Body)
		notes = r.URL.Query().Get("notes")
		prerelease = r.URL.Query().Get("prerelease")
		if refuse {
			data, _ := (&Error{Code: "invalid", Message: "the version 1.0.0 is already published"}).MarshalVT()
			w.WriteHeader(http.StatusConflict)
			_, _ = w.Write(data)
			return
		}
		data, _ := (&Release{Slug: "hello", Version: "1.0.0", ReviewState: ReviewState_REVIEW_STATE_APPROVED, Prerelease: prerelease == "1"}).MarshalVT()
		_, _ = w.Write(data)
	}))
	defer service.Close()

	// Save a valid session for the service.
	sessionFile := filepath.Join(t.TempDir(), "session.json")
	saved, _ := (&Session{Origin: service.URL, Token: "saved", ExpiresAt: time.Now().Add(time.Hour).UnixMilli()}).MarshalJSON()
	if err := os.WriteFile(sessionFile, saved, 0o600); err != nil {
		t.Fatal(err)
	}
	hyperline := &Hyperline{Origin: service.URL, SessionFile: sessionFile, Output: io.Discard}

	// Publish once and check what the service received.
	release, err := hyperline.Publish(context.Background(), mod)
	if err != nil {
		t.Fatal(err)
	}
	if release.GetReviewState() != ReviewState_REVIEW_STATE_APPROVED {
		t.Errorf("review state %v, want approved", release.GetReviewState())
	}
	if !slices.Equal(uploaded, []string{"mod.json", "mod.wasm", "maps/hl_court.vpk", "panorama/layout/hello-1.0.0/panel.vxml_c"}) {
		t.Errorf("uploaded %v, want the manifest, entry, map and content only", uploaded)
	}
	if notes != mod.Notes {
		t.Errorf("notes %q, want %q", notes, mod.Notes)
	}
	if prerelease != "" {
		t.Errorf("a release asked for prerelease %q", prerelease)
	}

	// A prerelease asks for one.
	mod.Prerelease = true
	if release, err := hyperline.Publish(context.Background(), mod); err != nil || prerelease != "1" || !release.GetPrerelease() {
		t.Errorf("a prerelease asked for prerelease %q and got %v: %v", prerelease, release, err)
	}
	mod.Prerelease = false

	// A refusal returns the service's message.
	refuse = true
	if _, err := hyperline.Publish(context.Background(), mod); err == nil || err.Error() != "the version 1.0.0 is already published" {
		t.Errorf("refusal %v, want the service's message", err)
	}
}

// TestCheckOrigin accepts HTTPS and loopback HTTP only.
func TestCheckOrigin(t *testing.T) {
	for origin, ok := range map[string]bool{
		"https://hyperline.gg":      true,
		"http://127.0.0.1:8080":     true,
		"http://hyperline.gg":       false,
		"https://hyperline.gg/api":  false,
		"https://user@hyperline.gg": false,
	} {
		if err := checkOrigin(origin); (err == nil) != ok {
			t.Errorf("checkOrigin(%q) = %v", origin, err)
		}
	}
}

// tarNames lists the files in a gzip-compressed tar.
func tarNames(t *testing.T, body io.Reader) []string {
	// Open the gzip stream, then read each header to its end.
	t.Helper()
	compressed, err := gzip.NewReader(body)
	if err != nil {
		t.Fatal(err)
	}
	archive := tar.NewReader(compressed)
	var names []string
	for {
		header, err := archive.Next()
		if err == io.EOF {
			return names
		}
		if err != nil {
			t.Fatal(err)
		}
		names = append(names, header.Name)
	}
}
