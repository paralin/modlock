// Package binaryen installs Binaryen and optimizes WebAssembly modules with
// its wasm-opt.
package binaryen

import (
	"context"
	"path/filepath"
	"runtime"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// Version is the Binaryen release that optimizes modules.
const Version = "version_133"

// archives maps each system to the release asset holding its tools and the
// asset's SHA-256.
var archives = map[string]struct{ name, sum string }{
	"darwin/arm64":  {"binaryen-" + Version + "-arm64-macos.tar.gz", "ad66da82ac13f163e424b1643f16c6dfcccc98b5966296b43e52d3cab04f84a8"},
	"linux/amd64":   {"binaryen-" + Version + "-x86_64-linux.tar.gz", "2dc9c7813f5375db93d96ead4b78222fcc3e2677bbb832297af4797782a37489"},
	"linux/arm64":   {"binaryen-" + Version + "-aarch64-linux.tar.gz", "89c07ea56faf38d0fbecf36ca8ec0721756716185f265b568e133d427f299bf8"},
	"windows/amd64": {"binaryen-" + Version + "-x86_64-windows.tar.gz", "17a2cbeac6b5693c5fbafab3838d3c65fd9c1eb38b05f5baec6c657e8c84995b"},
}

// WasmOpt returns the path of wasm-opt for this system, downloading the
// release into the user cache the first time.
func WasmOpt(ctx context.Context) (string, error) {
	// Find this system's release.
	archive, ok := archives[runtime.GOOS+"/"+runtime.GOARCH]
	if !ok {
		return "", errors.Errorf("binaryen publishes no release for %s/%s", runtime.GOOS, runtime.GOARCH)
	}

	// Install it, checking its checksum.
	dir, err := fetch.Cache("binaryen", Version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		// Download the release and check it before unpacking.
		path := staging + ".tar.gz"
		url := "https://github.com/WebAssembly/binaryen/releases/download/" + Version + "/" + archive.name
		if err := fetch.File(ctx, url, path); err != nil {
			return err
		}
		if err := fetch.Verify(path, archive.sum); err != nil {
			return err
		}

		// Unpack the tools under the release's directory.
		return fetch.UntarTree(path, "binaryen-"+Version+"/", staging)
	})
	if err != nil {
		return "", errors.Wrap(err, "install binaryen")
	}
	name := "wasm-opt"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	return filepath.Join(dir, "bin", name), nil
}
