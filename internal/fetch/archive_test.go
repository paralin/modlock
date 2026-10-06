package fetch

import (
	"archive/tar"
	"compress/gzip"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

// writeTarGz writes a gzipped tar archive holding a directory, two regular
// files and a symbolic link, and returns its path.
func writeTarGz(t *testing.T) string {
	// Open a gzip stream into a new archive file.
	t.Helper()
	archive := filepath.Join(t.TempDir(), "archive.tar.gz")
	file, err := os.Create(archive)
	if err != nil {
		t.Fatal(err)
	}
	compressed := gzip.NewWriter(file)

	// Write each entry, with four bytes for each regular file.
	writer := tar.NewWriter(compressed)
	for _, header := range []*tar.Header{
		{Name: "bin/", Typeflag: tar.TypeDir, Mode: 0o755},
		{Name: "./bin/tool", Typeflag: tar.TypeReg, Mode: 0o755, Size: 4},
		{Name: "bin/link", Typeflag: tar.TypeSymlink, Linkname: "tool"},
		{Name: "readme", Typeflag: tar.TypeReg, Mode: 0o644, Size: 4},
	} {
		if err := writer.WriteHeader(header); err != nil {
			t.Fatal(err)
		}
		if header.Size != 0 {
			if _, err := writer.Write([]byte("data")); err != nil {
				t.Fatal(err)
			}
		}
	}

	// Flush the tar and gzip streams into the file.
	for _, closer := range []io.Closer{writer, compressed, file} {
		if err := closer.Close(); err != nil {
			t.Fatal(err)
		}
	}
	return archive
}

// TestWalkTar checks that the walk visits only regular files by cleaned name
// and that fs.SkipAll ends it without error.
func TestWalkTar(t *testing.T) {
	// Every regular file is visited with its mode and contents.
	archive := writeTarGz(t)
	var names []string
	err := WalkTar(archive, func(name string, mode fs.FileMode, body io.Reader) error {
		// Record the file after checking its contents and mode.
		data, err := io.ReadAll(body)
		if err != nil {
			return err
		}
		if string(data) != "data" {
			t.Errorf("%s holds %q", name, data)
		}
		if name == "bin/tool" && mode != 0o755 {
			t.Errorf("bin/tool has mode %v", mode)
		}
		names = append(names, name)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if want := []string{"bin/tool", "readme"}; !slices.Equal(names, want) {
		t.Fatalf("visited %v, want %v", names, want)
	}

	// SkipAll stops after the first file.
	visits := 0
	err = WalkTar(archive, func(string, fs.FileMode, io.Reader) error {
		visits++
		return fs.SkipAll
	})
	if err != nil || visits != 1 {
		t.Fatalf("SkipAll walk returned %v after %d visits", err, visits)
	}
}

// TestUntar checks that Untar writes the named file and reports a missing one.
func TestUntar(t *testing.T) {
	// The named file becomes an executable at target.
	archive := writeTarGz(t)
	target := filepath.Join(t.TempDir(), "tool")
	if err := Untar(archive, "bin/tool", target); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(target)
	if err != nil || string(data) != "data" {
		t.Fatalf("read %q, %v", data, err)
	}

	// A name the archive lacks is an error.
	if err := Untar(archive, "bin/missing", target); err == nil {
		t.Fatal("Untar found a file the archive lacks")
	}
}
