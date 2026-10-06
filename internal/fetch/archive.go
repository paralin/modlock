package fetch

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"

	"github.com/pkg/errors"
)

// Verify checks that the file at path has the SHA-256 sum.
func Verify(path, sum string) error {
	// Hash the whole file and compare it with the pinned sum.
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
	// Open the named entry and copy it out.
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
	found := false
	err := WalkTar(archive, func(entry string, _ fs.FileMode, body io.Reader) error {
		// Copy the named file and stop the walk.
		if entry != name {
			return nil
		}
		found = true
		if err := executable(target, body); err != nil {
			return err
		}
		return fs.SkipAll
	})
	if err == nil && !found {
		return errors.Errorf("%s holds no %s", filepath.Base(archive), name)
	}
	return err
}

// WalkTar calls visit with the cleaned slash-separated name, permission bits
// and contents of each regular file in the gzipped tar archive at archive, in
// archive order. It stops at the first error visit returns; fs.SkipAll stops
// the walk and returns nil.
func WalkTar(archive string, visit func(name string, mode fs.FileMode, body io.Reader) error) error {
	// Open the archive's tar stream.
	file, err := os.Open(archive)
	if err != nil {
		return err
	}
	defer file.Close()
	compressed, err := gzip.NewReader(file)
	if err != nil {
		return err
	}

	// Visit each regular file until the archive or the visitor ends the walk.
	reader := tar.NewReader(compressed)
	for {
		header, err := reader.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		if header.Typeflag != tar.TypeReg {
			continue
		}
		err = visit(path.Clean(header.Name), header.FileInfo().Mode().Perm(), reader)
		if errors.Is(err, fs.SkipAll) {
			return nil
		}
		if err != nil {
			return err
		}
	}
}

// executable copies source into a new executable file at target.
func executable(target string, source io.Reader) error {
	// Create the file with its directory, then copy the contents in.
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
