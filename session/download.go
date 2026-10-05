package session

import (
	"archive/zip"
	"context"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// HostArchive is the release asset that holds the Windows host.
const HostArchive = "modlock-host-windows-x64.zip"

// DownloadHost returns modlock-host.exe from the release tagged version,
// downloading and extracting it into the user cache the first time.
func DownloadHost(ctx context.Context, version string) (string, error) {
	if !strings.HasPrefix(version, "v") {
		return "", errors.New("this modlock build has no release; set MODLOCK_HOST to modlock-host.exe")
	}
	dir, err := fetch.Cache("host", version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		// Download the archive beside the final directory, then unpack it.
		archive := dir + ".zip"
		defer os.Remove(archive)
		if err := fetch.File(ctx, fetch.Release(version, HostArchive), archive); err != nil {
			return err
		}
		return extract(archive, staging)
	})
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "modlock-host.exe"), nil
}

// extract unpacks the zip archive at path into dir.
func extract(path, dir string) error {
	archive, err := zip.OpenReader(path)
	if err != nil {
		return errors.Wrap(err, "open the host archive")
	}
	defer archive.Close()
	for _, entry := range archive.File {
		// Refuse entries that would land outside dir.
		target := filepath.Join(dir, entry.Name)
		if !strings.HasPrefix(target, filepath.Clean(dir)+string(filepath.Separator)) {
			return errors.Errorf("the host archive has an unsafe entry %q", entry.Name)
		}
		if entry.FileInfo().IsDir() {
			continue
		}

		// Copy the entry.
		if err := extractFile(entry, target); err != nil {
			return err
		}
	}
	return nil
}

// extractFile copies one archive entry to target.
func extractFile(entry *zip.File, target string) error {
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return errors.Wrap(err, "extract the host")
	}
	source, err := entry.Open()
	if err != nil {
		return errors.Wrap(err, "extract the host")
	}
	defer source.Close()
	file, err := os.Create(target)
	if err != nil {
		return errors.Wrap(err, "extract the host")
	}
	defer file.Close()
	if _, err := io.Copy(file, source); err != nil {
		return errors.Wrap(err, "extract the host")
	}
	return file.Close()
}
