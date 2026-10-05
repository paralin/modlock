// Command hello answers /hello, as a new Go mod does.
package main

import "github.com/paralin/modlock/mod"

// init registers the command.
func init() {
	mod.Command("hello", func(p mod.Player, _ string) {
		_ = p.Chat("Hello!")
	})
}

// main never runs; the server calls the handlers.
func main() {}
