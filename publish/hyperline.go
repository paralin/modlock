// Package publish hands a built mod to a provider: hyperline.gg, an archive
// players unpack and run, or a GitHub release that holds the archive.
package publish

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"time"

	"github.com/pkg/errors"
)

// DefaultOrigin is the service modlock publish uses by default.
const DefaultOrigin = "https://hyperline.gg"

// releasesPath is where the service takes a release.
const releasesPath = "/api/mods/releases"

// devLobbyJoinPath is where the service names a dev lobby's host session.
const devLobbyJoinPath = "/api/dev-lobby/join"

// maxUploadBytes is the largest compressed mod the service takes.
const maxUploadBytes = 96 << 20

// maxResponseBytes bounds one response the command line reads.
const maxResponseBytes = 1 << 20

// contentType is the media type of every request and response body.
const contentType = "application/octet-stream"

// Hyperline publishes to hyperline.gg or another service with its API.
type Hyperline struct {
	// Origin is the service, such as DefaultOrigin.
	Origin string
	// SessionFile holds the saved sign-in, readable only by the creator.
	SessionFile string
	// Output receives the sign-in instructions.
	Output io.Writer
}

// Publish uploads mod with its notes, as a prerelease when it is one, and returns the release the service
// recorded. It signs in through the browser when no saved session is valid.
func (h *Hyperline) Publish(ctx context.Context, mod *Mod) (*Release, error) {
	// Pack the build for the service.
	body, err := packTar(mod)
	if err != nil {
		return nil, err
	}
	if len(body) > maxUploadBytes {
		return nil, errors.New("the built mod exceeds 96 MiB compressed")
	}

	// Upload it.
	release := &Release{}
	query := url.Values{"notes": {mod.Notes}}
	if mod.Prerelease {
		query.Set("prerelease", "1")
	}
	path := releasesPath + "?" + query.Encode()
	return release, h.signedSend(ctx, path, body, release)
}

// JoinDevLobby returns the host session of the dev lobby with id lobby, whose
// member the signed-in player is. It signs in through the browser when no
// saved session is valid.
func (h *Hyperline) JoinDevLobby(ctx context.Context, lobby string) (*DevLobbyHost, error) {
	body, err := (&DevLobbyJoin{Lobby: lobby}).MarshalVT()
	if err != nil {
		return nil, err
	}
	host := &DevLobbyHost{}
	return host, h.signedSend(ctx, devLobbyJoinPath, body, host)
}

// errSignIn reports that the service refused the session.
var errSignIn = errors.New("sign in again")

// signedSend posts body to path with the saved session and decodes the answer
// into out, signing in again once when the service refuses the session.
func (h *Hyperline) signedSend(ctx context.Context, path string, body []byte, out interface{ UnmarshalVT([]byte) error }) error {
	// Send with the saved session, or a new one.
	if err := checkOrigin(h.Origin); err != nil {
		return err
	}
	token, err := h.token(ctx, false)
	if err != nil {
		return err
	}
	err = h.send(ctx, path, token, body, out)

	// Sign in again once if the service refused it.
	if errors.Is(err, errSignIn) {
		if token, err = h.token(ctx, true); err != nil {
			return err
		}
		err = h.send(ctx, path, token, body, out)
	}
	return err
}

// send posts body to path on the service and decodes the answer into out.
func (h *Hyperline) send(ctx context.Context, path, token string, body []byte, out interface{ UnmarshalVT([]byte) error }) error {
	// Address the request to the service with the session, if any.
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, h.Origin+path, bytes.NewReader(body))
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", contentType)
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}

	// A redirect must not carry the session to another origin.
	client := &http.Client{Timeout: 10 * time.Minute, CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	response, err := client.Do(request)
	if err != nil {
		return errors.Wrap(err, "reach "+h.Origin)
	}
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, maxResponseBytes))
	if err != nil {
		return errors.Wrap(err, "read the answer from "+h.Origin)
	}

	// Decode the answer, or the service's explanation of a refusal.
	switch {
	case response.StatusCode == http.StatusOK:
		return errors.Wrap(out.UnmarshalVT(data), "decode the answer from "+h.Origin)
	case response.StatusCode == http.StatusUnauthorized && token != "":
		return errSignIn
	}
	refusal := &Error{}
	if refusal.UnmarshalVT(data) != nil || refusal.GetMessage() == "" {
		return errors.Errorf("%s answered %s", h.Origin, response.Status)
	}
	return errors.New(refusal.GetMessage())
}

// SavedSession returns the session saved at path for origin, or nil when
// none is saved, it is for another origin, or it has expired.
func SavedSession(path, origin string) *Session {
	// Read the saved session, then keep it only while it is usable here.
	saved := &Session{}
	data, err := os.ReadFile(path)
	if err != nil || saved.UnmarshalJSON(data) != nil {
		return nil
	}
	if saved.GetOrigin() != origin || saved.GetToken() == "" || saved.GetExpiresAt() <= time.Now().UnixMilli() {
		return nil
	}
	return saved
}

// token returns the saved session's credential, signing in when it is
// missing, expired, from another origin, or refused.
func (h *Hyperline) token(ctx context.Context, refused bool) (string, error) {
	// Reuse the saved session while the service still takes it.
	if saved := SavedSession(h.SessionFile, h.Origin); saved != nil && !refused {
		return saved.GetToken(), nil
	}
	session, err := h.SignIn(ctx)
	if err != nil {
		return "", err
	}
	return session.GetToken(), nil
}

// SignIn signs the creator in through the browser and saves the session for
// every later command, replacing any saved one.
func (h *Hyperline) SignIn(ctx context.Context) (*Session, error) {
	// Sign in only to a service that keeps the session private.
	if err := checkOrigin(h.Origin); err != nil {
		return nil, err
	}

	// Sign in, then save the session beside modlock's other state.
	session, err := h.signIn(ctx)
	if err != nil {
		return nil, err
	}
	data, err := session.MarshalJSON()
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(filepath.Dir(h.SessionFile), 0o700); err != nil {
		return nil, errors.Wrap(err, "save the sign-in")
	}
	if err := os.WriteFile(h.SessionFile, append(data, '\n'), 0o600); err != nil {
		return nil, errors.Wrap(err, "save the sign-in")
	}
	return session, nil
}

// SignOut removes the saved session. Signing out when signed out does nothing.
func (h *Hyperline) SignOut() error {
	if err := os.Remove(h.SessionFile); err != nil && !os.IsNotExist(err) {
		return errors.Wrap(err, "remove the sign-in")
	}
	return nil
}

// signIn sends the creator to the service's sign-in page and trades the
// code it hands back to a loopback listener for a session. A one-use state
// binds the callback to this attempt, and the verifier proves it to the
// service.
func (h *Hyperline) signIn(ctx context.Context) (*Session, error) {
	// Listen on loopback for the callback with fresh secrets for this attempt.
	ctx, cancel := context.WithTimeout(ctx, 5*time.Minute)
	defer cancel()
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return nil, errors.Wrap(err, "listen for the sign-in")
	}
	defer listener.Close()
	verifier, state := randomToken(), randomToken()
	challenge := sha256.Sum256([]byte(verifier))
	callback := "http://" + listener.Addr().String() + "/callback"

	// Take the first callback that carries this attempt's state.
	codes := make(chan string, 1)
	server := &http.Server{ReadHeaderTimeout: 5 * time.Second, Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		values := r.URL.Query()
		if r.URL.Path != "/callback" || subtle.ConstantTimeCompare([]byte(values.Get("state")), []byte(state)) != 1 || values.Get("code") == "" {
			http.Error(w, "This sign-in link is not for this modlock. Start publishing again.", http.StatusBadRequest)
			return
		}
		select {
		case codes <- values.Get("code"):
			_, _ = io.WriteString(w, "Signed in. Return to modlock; you can close this tab.\n")
		default:
			http.Error(w, "This sign-in link was already used.", http.StatusConflict)
		}
	})}
	go func() { _ = server.Serve(listener) }()
	defer server.Close()

	// Send the creator to the sign-in page, and print it in case no browser
	// opens.
	page := h.Origin + "/desktop/authorize?" + url.Values{
		"callback":  {callback},
		"challenge": {base64.RawURLEncoding.EncodeToString(challenge[:])},
		"state":     {state},
	}.Encode()
	_, _ = io.WriteString(h.Output, "Sign in to "+h.Origin+" in your browser:\n  "+page+"\n")
	_ = openURL(page)
	var code string
	select {
	case code = <-codes:
	case <-ctx.Done():
		return nil, errors.Wrap(ctx.Err(), "wait for the sign-in")
	}

	// Trade the code and verifier for a session.
	exchange, err := (&SignInExchange{Code: code, Verifier: verifier}).MarshalVT()
	if err != nil {
		return nil, err
	}
	signedIn := &SignedIn{}
	if err := h.send(ctx, "/api/identity/desktop/exchange", "", exchange, signedIn); err != nil {
		return nil, errors.Wrap(err, "finish the sign-in")
	}
	if signedIn.GetToken() == "" {
		return nil, errors.New("the sign-in returned no session")
	}
	_, _ = io.WriteString(h.Output, "Signed in as "+signedIn.GetAccount().GetDisplayName()+"\n")
	return &Session{
		Origin:    h.Origin,
		Token:     signedIn.GetToken(),
		ExpiresAt: signedIn.GetExpiresAt(),
		Name:      signedIn.GetAccount().GetDisplayName(),
	}, nil
}

// checkOrigin requires HTTPS, except on loopback for a local service.
func checkOrigin(origin string) error {
	parsed, err := url.Parse(origin)
	if err != nil || parsed.Host == "" || parsed.Path != "" || parsed.RawQuery != "" || parsed.User != nil {
		return errors.Errorf("%q is not a service origin such as %s", origin, DefaultOrigin)
	}
	if parsed.Scheme != "https" && !(parsed.Scheme == "http" && parsed.Hostname() == "127.0.0.1") {
		return errors.Errorf("%s must use https", origin)
	}
	return nil
}

// randomToken returns 256 random bits, URL-safe.
func randomToken() string {
	var token [32]byte
	_, _ = rand.Read(token[:])
	return base64.RawURLEncoding.EncodeToString(token[:])
}

// openURL opens page in the creator's browser.
func openURL(page string) error {
	switch runtime.GOOS {
	case "darwin":
		return exec.Command("open", page).Start()
	case "windows":
		return exec.Command("rundll32", "url.dll,FileProtocolHandler", page).Start()
	default:
		return exec.Command("xdg-open", page).Start()
	}
}

// packTar returns mod's files as a gzip-compressed tar.
func packTar(mod *Mod) ([]byte, error) {
	// Write each file into the tar inside the gzip stream.
	files, err := mod.Files()
	if err != nil {
		return nil, err
	}
	var buf bytes.Buffer
	compressed := gzip.NewWriter(&buf)
	archive := tar.NewWriter(compressed)
	for _, name := range files {
		data, err := os.ReadFile(filepath.Join(mod.Dir, filepath.FromSlash(name)))
		if err != nil {
			return nil, errors.Wrap(err, "pack the built mod")
		}
		header := &tar.Header{Name: name, Mode: 0o644, Size: int64(len(data)), ModTime: time.Now()}
		if err := archive.WriteHeader(header); err != nil {
			return nil, err
		}
		if _, err := archive.Write(data); err != nil {
			return nil, err
		}
	}

	// Close both writers so the stream ends cleanly.
	if err := archive.Close(); err != nil {
		return nil, err
	}
	if err := compressed.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}
