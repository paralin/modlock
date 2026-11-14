package console

import "github.com/paralin/modlock/mod"

// Setting is one console variable with the value Apply sets it to.
type Setting struct {
	// Name is the variable's name, such as sv_cheats.
	Name string
	// Value is the value as the console reads it, such as 1 for true.
	Value string
}

// To returns the setting of the variable to value, for Apply.
func (v Variable[T]) To(value T) Setting {
	return Setting{Name: v.Name, Value: format(value)}
}

// Apply sets each variable in settings, in order, and returns the names the
// host refused, so a mod can log them and play on. Each setting is one host
// call: apply settings when the mod starts or a world loads, not every frame.
func Apply(settings ...Setting) []string {
	var refused []string
	for _, setting := range settings {
		if mod.ServerCommand(setting.Name+" "+setting.Value) != nil {
			refused = append(refused, setting.Name)
		}
	}
	return refused
}

// Arena returns the settings that empty a map for a mode of its own. The
// server runs frames with no one connected, which it otherwise skips, and no
// solo bot match, King of the Hill, troopers, jungle camps or practice bots
// start, nor does a player start on a zipline. A mode applies them before its
// own settings, which may override them.
//
//	console.Apply(append(console.Arena(), console.CitadelAllowDuplicateHeroes.To(true))...)
func Arena() []Setting {
	return []Setting{
		SvHibernateWhenEmpty.To(false),
		CitadelSoloBotMatch.To(false),
		CitadelKothEnabled.To(false),
		CitadelTrooperSpawnEnabled.To(false),
		CitadelNeutralSpawnEnabled.To(false),
		CitadelSpawnPracticeBots.To(false),
		CitadelStartPlayersOnZipline.To(false),
	}
}
