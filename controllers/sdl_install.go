package controllers

import (
	"context"
	"path/filepath"

	"github.com/paralin/modlock/internal/fetch"
)

// SDLVersion is the SDL release whose Windows x64 SDL3.dll the reader loads
// when none sits beside the executable.
const SDLVersion = "3.4.20"

// SDLArchive is SDL's published Windows x64 archive of SDLVersion, and
// SDLArchiveSHA256 its pinned checksum, which every download must match.
const (
	SDLArchive       = "https://github.com/libsdl-org/SDL/releases/download/release-" + SDLVersion + "/SDL3-" + SDLVersion + "-win32-x64.zip"
	SDLArchiveSHA256 = "368bed8b24e44ad9ea372990814f2343fcfb9e468abb7f8f4a3bc146c7304752"
)

// InstallSDL downloads SDLArchive, checks it against SDLArchiveSHA256, and
// writes its SDL3.dll into dir, which it creates whole or not at all. It does
// nothing when dir exists. An application that ships SDL3.dll beside its
// executable calls it at build time; the Windows reader calls it on the PC
// that lacks one.
func InstallSDL(ctx context.Context, dir string) error {
	return fetch.Install(dir, func(staging string) error {
		// Download the archive beside the staging directory and check it.
		archive := staging + ".zip"
		if err := fetch.File(ctx, SDLArchive, archive); err != nil {
			return err
		}
		if err := fetch.Verify(archive, SDLArchiveSHA256); err != nil {
			return err
		}

		// Take the library out of it.
		return fetch.Unzip(archive, "SDL3.dll", filepath.Join(staging, "SDL3.dll"))
	})
}
