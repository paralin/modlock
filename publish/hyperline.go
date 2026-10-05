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

	"github.com/paralin/modlock/proto/modlock/publish"
	"github.com/pkg/errors"
)

// DefaultOrigin is the service modlock publish uses by default.
const DefaultOrigin = "https://hyperline.gg"

// releasesPath is where the service takes a release.
const releasesPath = "/api/mods/releases"

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

// Publish uploads mod and returns the release the service recorded. It signs
// in through the browser when no saved session is valid.
func (h *Hyperline) Publish(ctx context.Context, mod *Mod) (*publish.Release, error) {
	if err := checkOrigin(h.Origin); err != nil {
		return nil, err
	}
	body, err := packTar(mod)
	if err != nil {
		return nil, err
	}
	if len(body) > maxUploadBytes {
		return nil, errors.New("the built mod exceeds 96 MiB compressed")
	}

	// Upload with the saved session, signing in again once if it was refused.
	token, err := h.token(ctx, false)
	if err != nil {
		return nil, err
	}
	release, err := h.upload(ctx, token, body)
	if errors.Is(err, errSignIn) {
		if token, err = h.token(ctx, true); err != nil {
			return nil, err
		}
		release, err = h.upload(ctx, token, body)
	}
	return release, err
}

// errSignIn reports that the service refused the session.
var errSignIn = errors.New("sign in again")

// upload sends one release.
func (h *Hyperline) upload(ctx context.Context, token string, body []byte) (*publish.Release, error) {
	release := &publish.Release{}
	err := h.send(ctx, releasesPath, token, body, release)
	return release, err
}

// send posts body to path on the service and decodes the answer into out.
func (h *Hyperline) send(ctx context.Context, path, token string, body []byte, out interface{ UnmarshalVT([]byte) error }) error {
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
	refusal := &publish.Error{}
	if refusal.UnmarshalVT(data) != nil || refusal.GetMessage() == "" {
		return errors.Errorf("%s answered %s", h.Origin, response.Status)
	}
	return errors.New(refusal.GetMessage())
}

// token returns the saved session's credential, signing in when it is
// missing, expired, from another origin, or refused.
func (h *Hyperline) token(ctx context.Context, refused bool) (string, error) {
	saved := &publish.Session{}
	if data, err := os.ReadFile(h.SessionFile); err == nil && !refused && saved.UnmarshalJSON(data) == nil &&
		saved.GetOrigin() == h.Origin && saved.GetToken() != "" && saved.GetExpiresAt() > time.Now().UnixMilli() {
		return saved.GetToken(), nil
	}
	session, err := h.signIn(ctx)
	if err != nil {
		return "", err
	}
	data, err := session.MarshalJSON()
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(filepath.Dir(h.SessionFile), 0o700); err != nil {
		return "", errors.Wrap(err, "save the sign-in")
	}
	if err := os.WriteFile(h.SessionFile, append(data, '\n'), 0o600); err != nil {
		return "", errors.Wrap(err, "save the sign-in")
	}
	return session.GetToken(), nil
}

// signIn sends the creator to the service's sign-in page and trades the
// code it hands back to a loopback listener for a session. A one-use state
// binds the callback to this attempt, and the verifier proves it to the
// service.
func (h *Hyperline) signIn(ctx context.Context) (*publish.Session, error) {
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
	exchange, err := (&publish.SignInExchange{Code: code, Verifier: verifier}).MarshalVT()
	if err != nil {
		return nil, err
	}
	signedIn := &publish.SignedIn{}
	if err := h.send(ctx, "/api/identity/desktop/exchange", "", exchange, signedIn); err != nil {
		return nil, errors.Wrap(err, "finish the sign-in")
	}
	if signedIn.GetToken() == "" {
		return nil, errors.New("the sign-in returned no session")
	}
	_, _ = io.WriteString(h.Output, "Signed in as "+signedIn.GetAccount().GetDisplayName()+"\n")
	return &publish.Session{
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
	var buf bytes.Buffer
	compressed := gzip.NewWriter(&buf)
	archive := tar.NewWriter(compressed)
	for _, name := range mod.Files() {
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
	if err := archive.Close(); err != nil {
		return nil, err
	}
	if err := compressed.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}
