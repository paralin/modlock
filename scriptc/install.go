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
const Version = "0.4.6-paralin.2"

// asset is one file of the release and its SHA-256.
type asset struct{ name, sum string }

// compilers maps each system to the release asset holding its standalone
// compiler.
var compilers = map[string]asset{
	"darwin/arm64":  {"scriptc-" + Version + "-darwin-arm64.tar.gz", "da77ea9d6f53e31fc12bf82c829c3c5b126551b8a46c944cd3ac853b423d7874"},
	"linux/amd64":   {"scriptc-" + Version + "-linux-x64-gnu.tar.gz", "f45e8b6774681cdab0f1329fc9508646c8d36e59fcd9ab2fc0acf857ecf66d22"},
	"linux/arm64":   {"scriptc-" + Version + "-linux-arm64-gnu.tar.gz", "972a0125ccc9977bd8aba623484b02554f4ac4010a28e47e7eff5624c0d9e4f0"},
	"windows/amd64": {"scriptc-" + Version + "-win32-x64-msvc.tar.gz", "bf3a1ce9511fcb5086ac2b1bf62706089395eb327c642609eb0bfe5d0bad8c11"},
}

// runtimePack is the release asset holding the wasm32-wasi runtime pack, an
// npm package archive.
var runtimePack = asset{"scriptc-runtime-wasm32-wasi-" + Version + ".tgz", "ff8bd1eb4d5b0a671157f76dfb1bb54c429cf9989de8ebb79183e3493fa78640"}

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
