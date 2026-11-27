// Command hello answers /hello, as a new Go mod does.
package main

import "github.com/paralin/modlock/mod"

// init registers the command, and logs each player who joins.
func init() {
	mod.Command("hello", func(p mod.Player, _ string) {
		_ = p.Chat("Hello!")
	})
	mod.OnPlayerJoin(func(joined *mod.PlayerJoinedEvent) {
		mod.Log("joined " + joined.GetConnection().GetName())
	})
}

// main never runs; the server calls the handlers.
func main() {}
