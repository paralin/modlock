// Package npm downloads pinned npm packages into the user cache: the
// TypeScript checker and the runtime the TypeScript library imports.
package npm

import (
	"context"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// TsgoVersion is the TypeScript native preview that checks script mods and
// declares the library.
const TsgoVersion = "7.0.0-dev.20260707.2"

// Package returns the directory holding the npm package name at version,
// downloading it from the registry into the user cache the first time.
func Package(ctx context.Context, name, version string) (string, error) {
	dir, err := fetch.Cache("npm", name+"@"+version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		archive := dir + ".tgz"
		defer os.Remove(archive)
		url := "https://registry.npmjs.org/" + name + "/-/" + path.Base(name) + "-" + version + ".tgz"
		if err := fetch.File(ctx, url, archive); err != nil {
			return err
		}
		return extract(archive, staging)
	})
	if err != nil {
		return "", errors.Wrapf(err, "install %s", name)
	}
	return dir, nil
}

// Tsgo returns the TypeScript checker for this system.
func Tsgo(ctx context.Context) (string, error) {
	// Name this system's package as npm does.
	system := map[string]string{"windows": "win32"}[runtime.GOOS]
	if system == "" {
		system = runtime.GOOS
	}
	arch := map[string]string{"amd64": "x64"}[runtime.GOARCH]
	if arch == "" {
		arch = runtime.GOARCH
	}
	executable := "tsgo"
	if runtime.GOOS == "windows" {
		executable += ".exe"
	}

	dir, err := Package(ctx, "@typescript/native-preview-"+system+"-"+arch, TsgoVersion)
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "lib", executable), nil
}

// extract unpacks the regular files under package/ in the npm archive at
// archive into dir, keeping their paths and modes.
func extract(archive, dir string) error {
	return fetch.WalkTar(archive, func(name string, mode fs.FileMode, body io.Reader) error {
		name, ok := strings.CutPrefix(name, "package/")
		if !ok || !filepath.IsLocal(name) {
			return nil
		}
		return write(filepath.Join(dir, filepath.FromSlash(name)), body, mode|0o644)
	})
}

// write copies source into a new file at target.
func write(target string, source io.Reader, mode os.FileMode) error {
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return err
	}
	file, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
	if err != nil {
		return err
	}
	_, err = io.Copy(file, source)
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	return err
}
