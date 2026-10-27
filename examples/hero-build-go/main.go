// Command hero-build-go reads a hero's recommended build from the host's game
// data. Players type /build hero_inferno to see its categories and items.
package main

import (
	"strings"

	"github.com/paralin/modlock/mod"
)

// init registers the mod's handlers before the server starts it.
func init() {
	mod.Command("build", func(p mod.Player, args string) {
		hero := args
		if hero == "" {
			hero = "hero_inferno"
		}
		categories, err := mod.HeroBuild(hero)
		if err != nil || len(categories) == 0 {
			_ = p.Chat("No build for " + hero + ".")
			return
		}
		for _, category := range categories {
			names := make([]string, 0, len(category.GetItems()))
			for _, item := range category.GetItems() {
				names = append(names, item.GetItem())
			}
			_ = p.Chat(category.GetName() + ": " + strings.Join(names, ", "))
		}
	})
}

// main is required by Go and never runs; the server calls the handlers.
func main() {}
