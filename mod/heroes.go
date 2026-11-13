package mod

import "strings"

// Hero is one released hero: its game name, such as "hero_atlas", the title
// players read and its fixed id.
type Hero struct {
	// Name is the game's name for the hero, as SelectHero takes it.
	Name string
	// ID is the hero's fixed id, as Pawn.Hero reports it.
	ID uint32
	// Title is the hero's name as players read it.
	Title string
	// Portrait is the hero's small stock portrait, as an image's src.
	Portrait string
}

// Heroes lists every released hero alphabetically by title, with the game's
// name, fixed id and small portrait for each. The table follows the game's
// hero data; a hero the game adds or releases joins it in a Modlock release.
var Heroes = []Hero{
	{Name: "hero_atlas", ID: 6, Title: "Abrams", Portrait: "file://{images}/heroes/bull_sm.psd"},
	{Name: "hero_fencer", ID: 77, Title: "Apollo", Portrait: "file://{images}/heroes/fencer_sm.psd"},
	{Name: "hero_baba", ID: 88, Title: "Baba", Portrait: "file://{images}/heroes/baba_sm.psd"},
	{Name: "hero_bebop", ID: 15, Title: "Bebop", Portrait: "file://{images}/heroes/bebop_sm.psd"},
	{Name: "hero_punkgoat", ID: 72, Title: "Billy", Portrait: "file://{images}/heroes/punkgoat_sm.psd"},
	{Name: "hero_nano", ID: 16, Title: "Calico", Portrait: "file://{images}/heroes/nano_sm.psd"},
	{Name: "hero_unicorn", ID: 81, Title: "Celeste", Portrait: "file://{images}/heroes/unicorn_sm.psd"},
	{Name: "hero_deadpack", ID: 78, Title: "Deadman Danny", Portrait: "file://{images}/heroes/deadpack_sm.psd"},
	{Name: "hero_drifter", ID: 64, Title: "Drifter", Portrait: "file://{images}/heroes/drifter_sm.psd"},
	{Name: "hero_dynamo", ID: 11, Title: "Dynamo", Portrait: "file://{images}/heroes/sumo_sm.psd"},
	{Name: "hero_necro", ID: 76, Title: "Graves", Portrait: "file://{images}/heroes/necro_sm.psd"},
	{Name: "hero_orion", ID: 17, Title: "Grey Talon", Portrait: "file://{images}/heroes/archer_sm.psd"},
	{Name: "hero_haze", ID: 13, Title: "Haze", Portrait: "file://{images}/heroes/haze_sm.psd"},
	{Name: "hero_astro", ID: 14, Title: "Holliday", Portrait: "file://{images}/heroes/astro_sm.psd"},
	{Name: "hero_inferno", ID: 1, Title: "Infernus", Portrait: "file://{images}/heroes/inferno_sm.psd"},
	{Name: "hero_tengu", ID: 20, Title: "Ivy", Portrait: "file://{images}/heroes/tengu_sm.psd"},
	{Name: "hero_kelvin", ID: 12, Title: "Kelvin", Portrait: "file://{images}/heroes/kelvin_sm.psd"},
	{Name: "hero_ghost", ID: 4, Title: "Lady Geist", Portrait: "file://{images}/heroes/spectre_sm.psd"},
	{Name: "hero_lash", ID: 31, Title: "Lash", Portrait: "file://{images}/heroes/lash_sm.psd"},
	{Name: "hero_forge", ID: 8, Title: "McGinnis", Portrait: "file://{images}/heroes/engineer_sm.psd"},
	{Name: "hero_vampirebat", ID: 63, Title: "Mina", Portrait: "file://{images}/heroes/vampirebat_sm.psd"},
	{Name: "hero_mirage", ID: 52, Title: "Mirage", Portrait: "file://{images}/heroes/mirage_sm.psd"},
	{Name: "hero_krill", ID: 18, Title: "Mo & Krill", Portrait: "file://{images}/heroes/digger_sm.psd"},
	{Name: "hero_nurse", ID: 87, Title: "Nurse Harrow", Portrait: "file://{images}/heroes/nurse_sm.psd"},
	{Name: "hero_bookworm", ID: 67, Title: "Paige", Portrait: "file://{images}/heroes/bookworm_sm.psd"},
	{Name: "hero_chrono", ID: 10, Title: "Paradox", Portrait: "file://{images}/heroes/chrono_sm.psd"},
	{Name: "hero_synth", ID: 50, Title: "Pocket", Portrait: "file://{images}/heroes/synth_sm.psd"},
	{Name: "hero_ratking", ID: 84, Title: "Rat King", Portrait: "file://{images}/heroes/ratking_sm.psd"},
	{Name: "hero_familiar", ID: 79, Title: "Rem", Portrait: "file://{images}/heroes/familiar_sm.psd"},
	{Name: "hero_gigawatt", ID: 2, Title: "Seven", Portrait: "file://{images}/heroes/gigawatt_sm.psd"},
	{Name: "hero_shiv", ID: 19, Title: "Shiv", Portrait: "file://{images}/heroes/shiv_sm.psd"},
	{Name: "hero_werewolf", ID: 80, Title: "Silver", Portrait: "file://{images}/heroes/werewolf_sm.psd"},
	{Name: "hero_magician", ID: 60, Title: "Sinclair", Portrait: "file://{images}/heroes/magician_sm.psd"},
	{Name: "hero_chessmaster", ID: 85, Title: "Solomon", Portrait: "file://{images}/heroes/chessmaster_sm.psd"},
	{Name: "hero_doorman", ID: 69, Title: "The Doorman", Portrait: "file://{images}/heroes/doorman_sm.psd"},
	{Name: "hero_priest", ID: 65, Title: "Venator", Portrait: "file://{images}/heroes/priest_sm.psd"},
	{Name: "hero_frank", ID: 66, Title: "Victor", Portrait: "file://{images}/heroes/frank_sm.psd"},
	{Name: "hero_hornet", ID: 3, Title: "Vindicta", Portrait: "file://{images}/heroes/hornet_sm.png"},
	{Name: "hero_artist", ID: 86, Title: "Violet", Portrait: "file://{images}/heroes/artist_sm.psd"},
	{Name: "hero_viscous", ID: 35, Title: "Viscous", Portrait: "file://{images}/heroes/viscous_sm.psd"},
	{Name: "hero_viper", ID: 58, Title: "Vyper", Portrait: "file://{images}/heroes/kali_sm.psd"},
	{Name: "hero_warden", ID: 25, Title: "Warden", Portrait: "file://{images}/heroes/warden_sm.psd"},
	{Name: "hero_wraith", ID: 7, Title: "Wraith", Portrait: "file://{images}/heroes/wraith_sm.psd"},
	{Name: "hero_yamato", ID: 27, Title: "Yamato", Portrait: "file://{images}/heroes/yamato_sm.psd"},
}

// heroesByName holds each released hero under every name HeroNamed accepts,
// reduced as plain reduces them. Game names go in last, so a title that
// equals another hero's game name could never take it.
var heroesByName = indexHeroes()

// HeroNamed returns the released hero that name names exactly: its game name,
// such as "hero_nano", that name without "hero_", such as "nano", or its
// title in any case, with or without spaces and punctuation, such as "Calico"
// or "ladygeist".
// Unlike FindHero it matches no part of a name, so a misspelled name finds no
// hero rather than another one.
// Returns false when name names no released hero.
func HeroNamed(name string) (Hero, bool) {
	hero, ok := heroesByName[plain(name, true)]
	return hero, ok
}

// FindHero matches what a player typed, such as "geist", "Lady Geist" or
// "hero_ghost", to a hero: an exact game name or title first, then the first
// title that starts with or contains it.
func FindHero(typed string) (Hero, bool) {
	// Reduce what was typed to letters, digits and underscores.
	key := plain(typed, true)
	if key == "" {
		return Hero{}, false
	}

	// Compare it with each name and plain title, the closest match first.
	matches := []func(hero Hero, title string) bool{
		func(hero Hero, title string) bool { return hero.Name == key || title == key },
		func(_ Hero, title string) bool { return strings.HasPrefix(title, key) },
		func(_ Hero, title string) bool { return strings.Contains(title, key) },
	}
	for _, match := range matches {
		for _, hero := range Heroes {
			if match(hero, plain(hero.Title, false)) {
				return hero, true
			}
		}
	}
	return Hero{}, false
}

// plain reduces text to its lowercase ASCII letters and digits, and its
// underscores when underscores is true.
func plain(text string, underscores bool) string {
	return strings.Map(func(r rune) rune {
		switch {
		case r >= 'a' && r <= 'z', r >= '0' && r <= '9', r == '_' && underscores:
			return r
		}
		return -1
	}, strings.ToLower(text))
}

// heroName returns the game's name for the hero name names, as HeroNamed
// finds it, or name itself when it names no released hero, so the host's
// error still reports it. Every generated call that takes a hero resolves it
// through heroName.
func heroName(name string) string {
	if hero, ok := HeroNamed(name); ok {
		return hero.Name
	}
	return name
}

// indexHeroes returns Heroes by each name HeroNamed accepts: plain title, game
// name without its prefix, then game name, each later one taking precedence.
func indexHeroes() map[string]Hero {
	byName := make(map[string]Hero, 3*len(Heroes))
	for _, hero := range Heroes {
		byName[plain(hero.Title, false)] = hero
	}
	for _, hero := range Heroes {
		byName[strings.TrimPrefix(hero.Name, "hero_")] = hero
		byName[hero.Name] = hero
	}
	return byName
}
