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
const Version = "0.4.17-paralin.2"

// asset is one file of the release and its SHA-256.
type asset struct{ name, sum string }

// compilers maps each system to the release asset holding its standalone
// compiler.
var compilers = map[string]asset{
	"darwin/arm64":  {"scriptc-" + Version + "-darwin-arm64.tar.gz", "858057bc93bfdf50855abb3dfa3b8f8de1b2b7a46d1080b294e58804d9cff6f1"},
	"linux/amd64":   {"scriptc-" + Version + "-linux-x64-gnu.tar.gz", "e8a191b1fd4c52adfa91c02779261c1b6bde8905333bbb3668f4b3cb45d0ca86"},
	"linux/arm64":   {"scriptc-" + Version + "-linux-arm64-gnu.tar.gz", "42d055c83fb406ed729d144534c347db65d20a554f8d55cb640fae3c0cf294cc"},
	"windows/amd64": {"scriptc-" + Version + "-win32-x64-msvc.tar.gz", "a71fc9addd462df8b7be3e66c2e9b2da514df23432104fb8accf1b198539311f"},
}

// runtimePack is the release asset holding the wasm32-wasi runtime pack, an
// npm package archive.
var runtimePack = asset{"scriptc-runtime-wasm32-wasi-" + Version + ".tgz", "f82207ce4be7474d6b4dce5fd4d1ad24d20d940861d4cfa78cc1cf905e767011"}

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
