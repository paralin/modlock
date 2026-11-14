package selfupdate

import (
	"context"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// Channel names the line of builds modlock update follows.
type Channel string

const (
	// Stable follows the releases, such as v0.1.9.
	Stable Channel = "stable"
	// Master follows the master branch: the build of its newest commit.
	Master Channel = "master"
)

// ParseChannel returns the channel named name.
func ParseChannel(name string) (Channel, error) {
	switch Channel(name) {
	case Stable, Master:
		return Channel(name), nil
	}
	return "", errors.Errorf("%q is not a channel; choose %s or %s", name, Stable, Master)
}

// Latest returns the version of the channel's newest build.
func (c Channel) Latest(ctx context.Context) (string, error) {
	if c == Master {
		return fetch.LatestMaster(ctx)
	}
	return Latest(ctx)
}

// News reports whether latest, the channel's newest build, should replace
// current. A release replaces only an older release. A build of master
// replaces any other published version, since master has no order to read
// from a version. A development build is never replaced.
func (c Channel) News(latest, current string) bool {
	if c == Master {
		return fetch.Master(latest) && fetch.Published(current) && latest != current
	}
	return Newer(latest, current)
}

// cache names the file that keeps the channel's newest version.
func (c Channel) cache() (string, error) {
	if c == Master {
		return fetch.Cache("latest-master")
	}
	return fetch.Cache("latest")
}
