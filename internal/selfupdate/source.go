package selfupdate

import (
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// repository is the address the source build clones.
const repository = "https://github.com/paralin/modlock"

// sourceTools are the programs a source build runs, each with what it does.
var sourceTools = [][2]string{
	{"git", "fetches the source"},
	{"go", "builds the command line"},
	{"zig", "compiles the game server for Windows"},
	{"cmake", "configures the game server's build"},
	{"ninja", "runs the game server's build"},
}

// Build builds the newest commit of the master branch on this computer and
// replaces the executable at exe with it, writing the tools' output to
// output. It keeps the source and the build in the user cache, so a later
// build compiles only what changed, and installs the game server it builds
// where a session looks for that version's. It returns the version built,
// which is the one the master channel gives the same commit.
func Build(ctx context.Context, exe string, output io.Writer) (string, error) {
	// Name every missing tool at once.
	var missing []string
	for _, tool := range sourceTools {
		if _, err := exec.LookPath(tool[0]); err != nil {
			missing = append(missing, tool[0]+" ("+tool[1]+")")
		}
	}
	if len(missing) != 0 {
		return "", errors.Errorf("a source build needs %s on the PATH; modlock update --channel master installs the same build without them", strings.Join(missing, ", "))
	}

	// Keep the source in the cache.
	source, err := fetch.Cache("source")
	if err != nil {
		return "", err
	}
	run := func(name string, args ...string) error {
		// Run the tool in the source, for a program without cgo.
		command := exec.CommandContext(ctx, name, args...)
		command.Dir = source
		command.Stdout, command.Stderr = output, output
		command.Env = append(os.Environ(), "CGO_ENABLED=0")
		return errors.Wrapf(command.Run(), "%s %s", name, strings.Join(args, " "))
	}

	// Fetch only that commit, over whatever the directory held.
	if _, err := os.Stat(filepath.Join(source, ".git")); err != nil {
		if err := os.MkdirAll(source, 0o755); err != nil {
			return "", errors.Wrap(err, "make the source directory")
		}
		if err := run("git", "init", "--quiet"); err != nil {
			return "", err
		}
	}
	if err := run("git", "fetch", "--depth", "1", repository, "master"); err != nil {
		return "", err
	}
	if err := run("git", "checkout", "--quiet", "--force", "--detach", "FETCH_HEAD"); err != nil {
		return "", err
	}

	// Name the version after the commit.
	head := exec.CommandContext(ctx, "git", "rev-parse", "HEAD")
	head.Dir = source
	commit, err := head.Output()
	if err != nil {
		return "", errors.Wrap(err, "read the source's commit")
	}
	version := fetch.MasterVersion(strings.TrimSpace(string(commit)))

	// Build the game server into this version's place in the cache, unless
	// it is there already.
	host, err := fetch.Cache("host", version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(host, func(staging string) error {
		// Configure and build it for Windows with Zig, on half the cores.
		if err := run("go", "mod", "download", "github.com/aperturerobotics/protobuf", "github.com/aperturerobotics/abseil-cpp"); err != nil {
			return err
		}
		if err := run("cmake", "-S", ".", "-B", "build-host", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
			"-DCMAKE_TOOLCHAIN_FILE="+filepath.Join(source, "cmake", "zig-windows.cmake"), "-DBUILD_TESTING=OFF"); err != nil {
			return err
		}
		if err := run("cmake", "--build", "build-host", "--parallel", strconv.Itoa(max(runtime.NumCPU()/2, 1))); err != nil {
			return err
		}

		// Keep the installed programs, which are what a release ships.
		sdk := staging + ".sdk"
		if err := run("cmake", "--install", "build-host", "--prefix", sdk); err != nil {
			return err
		}
		return errors.Wrap(os.Rename(filepath.Join(sdk, "bin"), staging), "install the game server")
	})
	if err != nil {
		return "", err
	}

	// Build the library the command line embeds, then the command line,
	// beside exe so the swap stays within one filesystem.
	if err := run("go", "run", "./cmd/modlock-library"); err != nil {
		return "", err
	}
	ext := filepath.Ext(exe)
	staged := filepath.Join(filepath.Dir(exe), "."+strings.TrimSuffix(filepath.Base(exe), ext)+".update"+ext)
	defer os.Remove(staged)
	if err := run("go", "build", "-trimpath", "-ldflags", "-X main.Version="+version, "-o", staged, "./cmd/modlock"); err != nil {
		return "", err
	}
	return version, swap(staged, exe)
}
