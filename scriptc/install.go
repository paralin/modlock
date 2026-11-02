package scriptc

import (
	"context"
	"path/filepath"
	"runtime"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// Version is the scriptc release that builds script mods. It comes from the
// paralin/scriptc fork, which carries compiler fixes upstream has not
// released yet.
const Version = "0.4.7-paralin.2"

// asset is one file of the release and its SHA-256.
type asset struct{ name, sum string }

// compilers maps each system to the release asset holding its standalone
// compiler.
var compilers = map[string]asset{
	"darwin/arm64":  {"scriptc-" + Version + "-darwin-arm64.tar.gz", "44a531dbd1b9af48a1d9a33ecaac39206570a6d9f6d8a2ecb19506dc854b136c"},
	"linux/amd64":   {"scriptc-" + Version + "-linux-x64-gnu.tar.gz", "8a1b26965fef74d2bc2f912338a69276210f5d800784a2e569b073c94e9b37cf"},
	"linux/arm64":   {"scriptc-" + Version + "-linux-arm64-gnu.tar.gz", "421ee897b4ad66232aa646d135e81094c42024e78a2f899c7101ddb1bcee867e"},
	"windows/amd64": {"scriptc-" + Version + "-win32-x64-msvc.tar.gz", "b76093540b270fa2c441dc5d8d453e325a253f20d66a157411c61e93b919e0e0"},
}

// runtimePack is the release asset holding the wasm32-wasi runtime pack, an
// npm package archive.
var runtimePack = asset{"scriptc-runtime-wasm32-wasi-" + Version + ".tgz", "e5679a24c189349bf7185d27957688dcccf94f06cc7754c53167070710f1959b"}

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
		archive := staging + ".tar.gz"
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
