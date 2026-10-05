// Package luau holds the modlock library that Luau mods require as
// "@modlock", and downloads the analyzer that type checks them.
//
// modlock-sdkgen writes modlock/init.luau from the schema and library.luau;
// modlock/wire.luau is handwritten.
package luau

import (
	"context"
	"embed"
	"os"
	"path/filepath"
	"runtime"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// Library holds the library's modules under modlock/.
//
//go:embed modlock/*.luau
var Library embed.FS

// Version is the Luau release whose analyzer checks mods, matching the
// runtime's Luau.
const Version = "0.741"

// analyzers maps each system to the Luau release asset holding its
// analyzer and the asset's SHA-256. The macOS build is for Apple silicon.
var analyzers = map[string]struct{ asset, sum string }{
	"darwin":  {"luau-macos.zip", "839cc1de39b0f765fbaea8e89421c12acfed6bd3d8d2cc2a2d3fc64bdde32b0f"},
	"linux":   {"luau-ubuntu.zip", "134dc762ad26232af83e43f98dec03ff6030dd3a4452f9408b9d50ccea025503"},
	"windows": {"luau-windows.zip", "be90c3223f3dc26777ef234244c2b1eae16b3c574f1ab7d1446972f72c28cab1"},
}

// Analyzer returns luau-analyze for this system, downloading it into the
// user cache the first time.
func Analyzer(ctx context.Context) (string, error) {
	release, ok := analyzers[runtime.GOOS]
	if !ok {
		return "", errors.Errorf("Luau publishes no analyzer for %s", runtime.GOOS)
	}
	executable := "luau-analyze"
	if runtime.GOOS == "windows" {
		executable += ".exe"
	}
	dir, err := fetch.Cache("luau", Version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		archive := dir + ".zip"
		defer os.Remove(archive)
		url := "https://github.com/luau-lang/luau/releases/download/" + Version + "/" + release.asset
		if err := fetch.File(ctx, url, archive); err != nil {
			return err
		}
		if err := fetch.Verify(archive, release.sum); err != nil {
			return err
		}
		return fetch.Unzip(archive, executable, filepath.Join(staging, executable))
	})
	if err != nil {
		return "", errors.Wrap(err, "install luau-analyze")
	}
	return filepath.Join(dir, executable), nil
}
