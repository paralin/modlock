package fetch

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"os"
	"path/filepath"

	"github.com/pkg/errors"
)

// Verify checks that the file at path has the SHA-256 sum.
func Verify(path, sum string) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	hash := sha256.New()
	if _, err := io.Copy(hash, file); err != nil {
		return err
	}
	if got := hex.EncodeToString(hash.Sum(nil)); got != sum {
		return errors.Errorf("%s has SHA-256 %s, not the pinned %s", filepath.Base(path), got, sum)
	}
	return nil
}

// Unzip writes the zip archive's file name to target as an executable.
func Unzip(archive, name, target string) error {
	reader, err := zip.OpenReader(archive)
	if err != nil {
		return err
	}
	defer reader.Close()
	source, err := reader.Open(name)
	if err != nil {
		return err
	}
	defer source.Close()
	return executable(target, source)
}

// Untar writes the gzipped tar archive's file name to target as an
// executable.
func Untar(archive, name, target string) error {
	file, err := os.Open(archive)
	if err != nil {
		return err
	}
	defer file.Close()
	compressed, err := gzip.NewReader(file)
	if err != nil {
		return err
	}
	reader := tar.NewReader(compressed)
	for {
		header, err := reader.Next()
		if err == io.EOF {
			return errors.Errorf("%s holds no %s", filepath.Base(archive), name)
		}
		if err != nil {
			return err
		}
		if header.Name == name && header.Typeflag == tar.TypeReg {
			return executable(target, reader)
		}
	}
}

// executable copies source into a new executable file at target.
func executable(target string, source io.Reader) error {
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return err
	}
	file, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o755)
	if err != nil {
		return err
	}
	if _, err := io.Copy(file, source); err != nil {
		file.Close()
		return err
	}
	return file.Close()
}
