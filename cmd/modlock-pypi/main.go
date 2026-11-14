// Command modlock-pypi assembles the Python wheels of a Modlock release: one
// wheel per system, each holding that system's command line as a script, so
// pip install modlock puts modlock on the PATH pip manages:
//
//	modlock-pypi -version v0.2.0 -dist dist -out dist/pypi
//
// -dist holds the release's command lines, named modlock-<os>-<arch>. The
// wheels go in -out, ready to upload. A wheel holds no Python module: a mod
// imports the library the command line installs in its project.
package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// system is one system the release builds the command line for.
type system struct {
	// release is the command line's file name in the release.
	release string
	// script is its name once installed.
	script string
	// platform is the wheel's platform tag.
	platform string
}

// systems are the systems the release workflow builds. The command lines link
// no C library, so one Linux wheel serves both glibc and musl.
var systems = []system{
	{"modlock-darwin-arm64", "modlock", "macosx_11_0_arm64"},
	{"modlock-darwin-amd64", "modlock", "macosx_10_12_x86_64"},
	{"modlock-linux-amd64", "modlock", "manylinux_2_17_x86_64.manylinux2014_x86_64.musllinux_1_1_x86_64"},
	{"modlock-linux-arm64", "modlock", "manylinux_2_17_aarch64.manylinux2014_aarch64.musllinux_1_1_aarch64"},
	{"modlock-windows-amd64.exe", "modlock.exe", "win_amd64"},
}

// summary is the package's page on the index, in Markdown.
const summary = `# modlock

Build, test and publish mods for Deadlock. This package installs the
` + "`modlock`" + ` command line:

` + "```sh" + `
pip install modlock
modlock new
` + "```" + `

See https://github.com/paralin/modlock for the guide.
`

func main() {
	// Read the release and the directories from the command line.
	tag := flag.String("version", "", "release tag, such as v0.2.0")
	dist := flag.String("dist", "dist", "directory holding the release's command lines")
	out := flag.String("out", filepath.Join("dist", "pypi"), "directory to write the wheels into")
	flag.Parse()
	version, ok := strings.CutPrefix(*tag, "v")
	if !ok {
		fmt.Fprintln(os.Stderr, "modlock-pypi: -version names a release tag, such as v0.2.0")
		os.Exit(2)
	}

	// Write a wheel per system.
	if err := os.MkdirAll(*out, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, "modlock-pypi:", err)
		os.Exit(1)
	}
	for _, s := range systems {
		if err := writeWheel(version, *dist, *out, s); err != nil {
			fmt.Fprintln(os.Stderr, "modlock-pypi:", err)
			os.Exit(1)
		}
	}
}

// writeWheel writes the wheel of the release at version for s to out, with
// the command line from dist.
func writeWheel(version, dist, out string, s system) error {
	// Read the command line and the license.
	binary, err := os.ReadFile(filepath.Join(dist, s.release))
	if err != nil {
		return err
	}
	license, err := os.ReadFile("LICENSE")
	if err != nil {
		return err
	}

	// Describe the package and the wheel.
	tag := "py3-none-" + s.platform
	info := "modlock-" + version + ".dist-info/"
	metadata := "Metadata-Version: 2.1\n" +
		"Name: modlock\n" +
		"Version: " + version + "\n" +
		"Summary: Build, test and publish mods for Deadlock.\n" +
		"Home-page: https://github.com/paralin/modlock\n" +
		"License: MIT\n" +
		"Project-URL: Source, https://github.com/paralin/modlock\n" +
		"Requires-Python: >=3.8\n" +
		"Description-Content-Type: text/markdown\n\n" + summary
	wheel := "Wheel-Version: 1.0\nGenerator: modlock-pypi\nRoot-Is-Purelib: false\n"
	for _, platform := range strings.Split(s.platform, ".") {
		wheel += "Tag: py3-none-" + platform + "\n"
	}

	// List the wheel's files. The installer copies a script to the PATH and
	// keeps its executable bit.
	files := []struct {
		name string
		data []byte
		mode os.FileMode
	}{
		{"modlock-" + version + ".data/scripts/" + s.script, binary, 0o755},
		{info + "METADATA", []byte(metadata), 0o644},
		{info + "WHEEL", []byte(wheel), 0o644},
		{info + "licenses/LICENSE", license, 0o644},
	}

	// Write each file, and note it with its digest for the record.
	var archive bytes.Buffer
	writer := zip.NewWriter(&archive)
	var record strings.Builder
	for _, file := range files {
		header := &zip.FileHeader{Name: file.name, Method: zip.Deflate}
		header.SetMode(file.mode)
		entry, err := writer.CreateHeader(header)
		if err != nil {
			return err
		}
		if _, err := entry.Write(file.data); err != nil {
			return err
		}
		digest := sha256.Sum256(file.data)
		fmt.Fprintf(&record, "%s,sha256=%s,%d\n", file.name, base64.RawURLEncoding.EncodeToString(digest[:]), len(file.data))
	}

	// Write the record, which lists itself without a digest, and the wheel.
	record.WriteString(info + "RECORD,,\n")
	entry, err := writer.Create(info + "RECORD")
	if err != nil {
		return err
	}
	if _, err := entry.Write([]byte(record.String())); err != nil {
		return err
	}
	if err := writer.Close(); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(out, "modlock-"+version+"-"+tag+".whl"), archive.Bytes(), 0o644)
}
