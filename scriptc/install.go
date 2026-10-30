package scriptc

import (
	"context"
	"os"
	"path/filepath"
	"runtime"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// Version is the scriptc release that builds script mods. It comes from the
// paralin/scriptc fork, which carries compiler fixes upstream has not
// released yet.
const Version = "0.4.6-paralin.1"

// asset is one file of the release and its SHA-256.
type asset struct{ name, sum string }

// compilers maps each system to the release asset holding its standalone
// compiler.
var compilers = map[string]asset{
	"darwin/arm64":  {"scriptc-" + Version + "-darwin-arm64.tar.gz", "5b64734f276a3d6a776a84f4ef4bcd1ae7c5e19736afefda4040278086b4f3d8"},
	"linux/amd64":   {"scriptc-" + Version + "-linux-x64-gnu.tar.gz", "07515ed9e54551a0126047884eaa255ad6db581b4a99b94e12a39a90d7daedee"},
	"linux/arm64":   {"scriptc-" + Version + "-linux-arm64-gnu.tar.gz", "4a7631b3a1bc3f3a64a2d7a3d61a2047985b287531cdaf025362f227761831ef"},
	"windows/amd64": {"scriptc-" + Version + "-win32-x64-msvc.tar.gz", "9837a3f4b4fad46ae8c531db3bf0b77bf92f65bcbb85ccbcfc6e49fdf5732b0b"},
}

// runtimePack is the release asset holding the wasm32-wasi runtime pack, an
// npm package archive.
var runtimePack = asset{"scriptc-runtime-wasm32-wasi-" + Version + ".tgz", "e9a18582e428ac9ac6da830139f67ecb8e4144f8d302d1c75949ceb7b739a71d"}

// Compiler returns the scriptc compiler for this system and the runtime pack
// it links WebAssembly modules against.
func Compiler(ctx context.Context) (compiler, pack string, err error) {
	// Install this system's compiler and the runtime pack.
	release, ok := compilers[runtime.GOOS+"/"+runtime.GOARCH]
	if !ok {
		return "", "", errors.Errorf("scriptc publishes no compiler for %s/%s", runtime.GOOS, runtime.GOARCH)
	}
	dir, err := install(ctx, "compiler", release, "")
	if err != nil {
		return "", "", err
	}
	pack, err = install(ctx, "runtime-wasm32-wasi", runtimePack, "package/")
	if err != nil {
		return "", "", err
	}
	executable := "scriptc"
	if runtime.GOOS == "windows" {
		executable += ".exe"
	}
	return filepath.Join(dir, "bin", executable), pack, nil
}

// install returns the directory holding the files under prefix in the
// release asset, downloading it into the user cache the first time.
func install(ctx context.Context, name string, release asset, prefix string) (string, error) {
	dir, err := fetch.Cache("scriptc", Version, name)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		archive := dir + ".tar.gz"
		defer os.Remove(archive)
		url := "https://github.com/paralin/scriptc/releases/download/v" + Version + "/" + release.name
		if err := fetch.File(ctx, url, archive); err != nil {
			return err
		}
		if err := fetch.Verify(archive, release.sum); err != nil {
			return err
		}
		return fetch.UntarTree(archive, prefix, staging)
	})
	if err != nil {
		return "", errors.Wrapf(err, "install scriptc %s", name)
	}
	return dir, nil
}
