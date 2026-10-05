// Package python holds the modlock library that Python mods import as
// modlock, and downloads the Pyright checker and the Node.js that runs it.
//
// modlock-sdkgen writes modlock/__init__.py from the schema and library.py;
// modlock/wire.py and the runtime bridge's stub _modlock.pyi are
// handwritten.
package python

import (
	"context"
	"embed"
	"os"
	"path/filepath"
	"runtime"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/paralin/modlock/internal/npm"
	"github.com/pkg/errors"
)

// Library holds the library's package under modlock/ and the stub
// _modlock.pyi.
//
//go:embed modlock/*.py _modlock.pyi
var Library embed.FS

// PyrightVersion is the Pyright release that checks mods.
const PyrightVersion = "1.1.414"

// NodeVersion is the Node.js release that runs Pyright.
const NodeVersion = "24.21.0"

// nodes maps each system and architecture to its Node.js archive's platform
// and SHA-256.
var nodes = map[string]struct{ platform, sum string }{
	"darwin/amd64":  {"darwin-x64", "1462cb3b3046b815cf8ea436d3da450ec1a9f11dac7e5a46b0ada5305d7e8097"},
	"darwin/arm64":  {"darwin-arm64", "bed7eea5325e1108f32ce5228ddd6a5f0f08a499ee42aa7442aea583702f6057"},
	"linux/amd64":   {"linux-x64", "6e1db87ef58b8819e5d5402eff1536491b18edd8eb7bee5ef7897876e88dc5ff"},
	"linux/arm64":   {"linux-arm64", "724282c3b43aec998aa9527380465b45d229e021b58035f5f4f63095eabfe5d5"},
	"windows/amd64": {"win-x64", "158f7685b44de51f6c0df1d153526cbcd3e1bc739a8dfc607721cef75de9e541"},
	"windows/arm64": {"win-arm64", "8779b1bde1d39f8d420e3b57aa657b39891af434d3de44a919044cec06785921"},
}

// Checker returns Node.js and the Pyright script it runs, downloading both
// into the user cache the first time.
func Checker(ctx context.Context) (node, pyright string, err error) {
	node, err = Node(ctx)
	if err != nil {
		return "", "", err
	}
	dir, err := npm.Package(ctx, "pyright", PyrightVersion)
	if err != nil {
		return "", "", err
	}
	return node, filepath.Join(dir, "index.js"), nil
}

// Node returns the Node.js executable for this system.
func Node(ctx context.Context) (string, error) {
	release, ok := nodes[runtime.GOOS+"/"+runtime.GOARCH]
	if !ok {
		return "", errors.Errorf("Node.js publishes no build for %s/%s", runtime.GOOS, runtime.GOARCH)
	}
	base := "node-v" + NodeVersion + "-" + release.platform
	archive, name, executable := base+".tar.gz", base+"/bin/node", "node"
	unpack := fetch.Untar
	if runtime.GOOS == "windows" {
		archive, name, executable = base+".zip", base+"/node.exe", "node.exe"
		unpack = fetch.Unzip
	}
	dir, err := fetch.Cache("node", NodeVersion)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		downloaded := dir + filepath.Ext(archive)
		defer os.Remove(downloaded)
		url := "https://nodejs.org/dist/v" + NodeVersion + "/" + archive
		if err := fetch.File(ctx, url, downloaded); err != nil {
			return err
		}
		if err := fetch.Verify(downloaded, release.sum); err != nil {
			return err
		}
		return unpack(downloaded, name, filepath.Join(staging, executable))
	})
	if err != nil {
		return "", errors.Wrap(err, "install Node.js")
	}
	return filepath.Join(dir, executable), nil
}
