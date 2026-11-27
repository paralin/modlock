package main

import (
	"context"
	"os"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/peernet"
	modpublish "github.com/paralin/modlock/publish"
)

// loginCommand signs modlock in to Hyperline, so host, join and publish route
// through the site.
func loginCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "login",
		Usage: "sign in to hyperline.gg through the browser, so host, join and publish go through the site",
		Flags: []cli.Flag{
			&cli.StringFlag{Name: "origin", Value: modpublish.DefaultOrigin, Usage: "the Hyperline service to sign in to"},
		},
		Action: func(c *cli.Context) error {
			// Sign in and save the session.
			service, err := hyperlineService(c.String("origin"))
			if err != nil {
				return err
			}
			session, err := service.SignIn(c.Context)
			if err != nil {
				return err
			}

			// Have the daemon's peer link through the site with it.
			out.note("signed in as", session.GetName())
			return wakePeer(c.Context)
		},
	}
}

// logoutCommand removes the saved Hyperline sign-in.
func logoutCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "logout",
		Usage: "remove the saved hyperline.gg sign-in; host and join then link peers directly",
		Action: func(c *cli.Context) error {
			// Remove the session.
			service, err := hyperlineService(modpublish.DefaultOrigin)
			if err != nil {
				return err
			}
			if err := service.SignOut(); err != nil {
				return err
			}

			// Have the daemon's peer drop the old sign-in's link.
			out.note("signed out")
			return wakePeer(c.Context)
		},
	}
}

// hyperlineService returns the Hyperline service at origin with the saved
// sign-in, printing the sign-in's own prompts.
func hyperlineService(origin string) (*modpublish.Hyperline, error) {
	sessionFile, err := hyperlineSessionFile()
	if err != nil {
		return nil, err
	}
	return &modpublish.Hyperline{Origin: origin, SessionFile: sessionFile, Output: os.Stderr}, nil
}

// wakePeer has the daemon's peer read the saved sign-in again, so a running
// daemon links with a new sign-in or drops a removed one at once.
func wakePeer(ctx context.Context) error {
	// Ask the daemon for its peer, which wakes the peer's admission.
	client, err := connectDaemon(ctx)
	if err != nil {
		return err
	}
	defer client.Close()
	_, err = peernet.NewSRPCPeerNetClient(client.RPC()).GetPeer(ctx, &peernet.GetPeerRequest{})
	return err
}
