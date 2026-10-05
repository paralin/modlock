// Package fetch downloads the tools and releases the modlock command needs
// into the user cache.
package fetch

import (
	"context"
	"io"
	"net/http"
	"os"
	"path/filepath"

	"github.com/pkg/errors"
)

// Release returns the address of asset in the Modlock release tagged version.
func Release(version, asset string) string {
	return "https://github.com/paralin/modlock/releases/download/" + version + "/" + asset
}

// Cache returns the directory under the user cache that holds name.
func Cache(name ...string) (string, error) {
	cache, err := os.UserCacheDir()
	if err != nil {
		return "", errors.Wrap(err, "locate the cache directory")
	}
	return filepath.Join(append([]string{cache, "modlock"}, name...)...), nil
}

// File writes the body at url to path, creating its directory.
func File(ctx context.Context, url, path string) error {
	// Request the file.
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return errors.Wrapf(err, "download %s", url)
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		return errors.Wrapf(err, "download %s", url)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return errors.Errorf("download %s: %s", url, response.Status)
	}

	// Write it to path.
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return errors.Wrapf(err, "download %s", url)
	}
	file, err := os.Create(path)
	if err != nil {
		return errors.Wrapf(err, "download %s", url)
	}
	defer file.Close()
	if _, err := io.Copy(file, response.Body); err != nil {
		return errors.Wrapf(err, "download %s", url)
	}
	return file.Close()
}

// Install fills dir once: it runs fill on a staging directory beside dir and
// moves the result into place whole, so an interrupted install never looks
// complete. It does nothing when dir exists.
func Install(dir string, fill func(staging string) error) error {
	if _, err := os.Stat(dir); err == nil {
		return nil
	}
	staging := dir + ".tmp"
	_ = os.RemoveAll(staging)
	if err := fill(staging); err != nil {
		_ = os.RemoveAll(staging)
		return err
	}
	if err := os.Rename(staging, dir); err != nil {
		_ = os.RemoveAll(staging)
		return errors.Wrapf(err, "install %s", filepath.Base(dir))
	}
	return nil
}
