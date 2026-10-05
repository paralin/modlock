package publish

import (
	"archive/zip"
	"context"
	"io"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// windowsCommand is the release asset that holds the Windows command line.
const windowsCommand = "modlock-windows-amd64.exe"

// playScript runs the mod from the unpacked archive. The command line
// downloads the matching host the first time.
const playScript = "@echo off\r\ncd /d \"%~dp0\"\r\nmodlock.exe play mod\r\nif errorlevel 1 pause\r\n"

// Archive writes a zip players unpack and run on Windows: Play.cmd, the
// command line of release version, and mod under mod/, in one folder named
// for the mod. It returns the archive's path in dir.
func Archive(ctx context.Context, mod *Mod, version, dir string) (string, error) {
	command, err := WindowsCommand(ctx, version)
	if err != nil {
		return "", err
	}
	folder := mod.Manifest.GetSlug() + "-" + mod.Manifest.GetVersion()
	target := filepath.Join(dir, folder+".zip")
	file, err := os.Create(target + ".tmp")
	if err != nil {
		return "", errors.Wrap(err, "write the archive")
	}
	defer os.Remove(target + ".tmp")
	defer file.Close()

	// Write the script, the command line and the mod.
	archive := zip.NewWriter(file)
	if err := addFile(archive, path.Join(folder, "Play.cmd"), strings.NewReader(playScript)); err != nil {
		return "", err
	}
	if err := addPath(archive, path.Join(folder, "modlock.exe"), command); err != nil {
		return "", err
	}
	for _, name := range mod.Files() {
		if err := addPath(archive, path.Join(folder, "mod", name), filepath.Join(mod.Dir, filepath.FromSlash(name))); err != nil {
			return "", err
		}
	}

	// Finish the archive, then replace any earlier one.
	if err := archive.Close(); err != nil {
		return "", errors.Wrap(err, "write the archive")
	}
	if err := file.Close(); err != nil {
		return "", errors.Wrap(err, "write the archive")
	}
	return target, errors.Wrap(os.Rename(target+".tmp", target), "write the archive")
}

// addFile adds one compressed file to archive.
func addFile(archive *zip.Writer, name string, content io.Reader) error {
	writer, err := archive.Create(name)
	if err != nil {
		return errors.Wrap(err, "write the archive")
	}
	_, err = io.Copy(writer, content)
	return errors.Wrap(err, "write the archive")
}

// addPath adds the file at source to archive as name.
func addPath(archive *zip.Writer, name, source string) error {
	file, err := os.Open(source)
	if err != nil {
		return errors.Wrap(err, "write the archive")
	}
	defer file.Close()
	return addFile(archive, name, file)
}

// WindowsCommand returns the Windows command line of release version: this
// executable on Windows, or the release's copy, downloaded into the user
// cache the first time.
func WindowsCommand(ctx context.Context, version string) (string, error) {
	if runtime.GOOS == "windows" && runtime.GOARCH == "amd64" {
		return os.Executable()
	}
	if !strings.HasPrefix(version, "v") {
		return "", errors.New("this modlock build has no release, so it has no Windows command line to pack")
	}
	dir, err := fetch.Cache("command", version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		return fetch.File(ctx, fetch.Release(version, windowsCommand), filepath.Join(staging, "modlock.exe"))
	})
	return filepath.Join(dir, "modlock.exe"), err
}
