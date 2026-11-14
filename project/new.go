package project

import (
	"context"
	"encoding/json"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/js"
	"github.com/paralin/modlock/luau"
	"github.com/paralin/modlock/python"
	"github.com/paralin/modlock/src/wasm"
	"github.com/pkg/errors"
)

// Module is the Go module of the Modlock library mods import.
const Module = "github.com/paralin/modlock"

// Extension is the identifier of the Modlock extension for VS Code and the
// editors built on it, in the Visual Studio Marketplace and Open VSX.
const Extension = "modlock.modlock"

// LanguageExtension returns the identifier of the editor extension that reads
// language's sources, or "" when VS Code reads them itself, as it does
// TypeScript and JavaScript.
func LanguageExtension(language wasm.Manifest_Language) string {
	switch language {
	case wasm.Manifest_LANGUAGE_PYTHON:
		return "ms-python.python"
	case wasm.Manifest_LANGUAGE_LUAU:
		return "johnnymorganz.luau-lsp"
	case wasm.Manifest_LANGUAGE_GO:
		return "golang.go"
	default:
		return ""
	}
}

// editorRecommendations returns a project's .vscode/extensions.json, which
// asks the editor to offer the Modlock extension and language's own.
func editorRecommendations(language wasm.Manifest_Language) string {
	recommended := []string{Extension}
	if id := LanguageExtension(language); id != "" {
		recommended = append(recommended, id)
	}
	data, _ := json.MarshalIndent(map[string][]string{"recommendations": recommended}, "", "  ")
	return string(data) + "\n"
}

// goMain is a new Go mod's main.go. Like every starter it sets the server
// up, seats each player on a team with a hero, welcomes them
// and answers /hello.
const goMain = `// Command {{slug}} is a Modlock mod.
package main

import (
	"time"

	"github.com/paralin/modlock/mod"
	"github.com/paralin/modlock/mod/console"
)

// init registers the mod's handlers before the server starts it.
func init() {
	// Set the server up when the mod starts: no troopers in the lanes or
	// creeps in the jungle, and any number of players on the same hero.
	mod.OnStart(func(args []string) {
		_ = console.CitadelTrooperSpawnEnabled.Set(false)
		_ = console.CitadelNeutralSpawnEnabled.Set(false)
		_ = console.CitadelAllowDuplicateHeroes.Set(true)
	})

	// Bring a fallen hero back after 3 seconds.
	mod.SetRespawnDelay(3 * time.Second)

	// Give each player who joins a hero on the smaller team, skipping the
	// game's team and hero screens, and welcome them. Hero: "haze" names the
	// hero and Team: 2 the team.
	mod.OnPlayerJoin(func(joined *mod.PlayerJoinedEvent) {
		connection := joined.GetConnection()
		player := mod.PlayerOf(connection.GetPlayer())
		player.Seat(mod.Seating{Hero: mod.AnyHero})
		_ = player.Announce({{name}}, "Welcome, "+connection.GetName()+".")
	})

	// Greet each player who types /hello in chat.
	mod.Command("hello", func(p mod.Player, args string) {
		_ = p.Chat({{greeting}})
	})
}

// main is required by Go and never runs; the server calls the handlers.
func main() {}
`

// scriptMainSource is a new TypeScript or JavaScript mod's entry.
const scriptMainSource = `import { command, onPlayerJoin, onStart, setRespawnDelay } from 'modlock'
import { apply } from 'modlock/console'

// Set the server up when the mod starts: no troopers in the lanes or creeps
// in the jungle, and any number of players on the same hero.
onStart(() => {
  apply({
    citadel_trooper_spawn_enabled: false,
    citadel_neutral_spawn_enabled: false,
    citadel_allow_duplicate_heroes: true,
  })
})

// Bring a fallen hero back after 3 seconds.
setRespawnDelay(3)

// Give each player who joins a hero on the smaller team, skipping the game's
// team and hero screens, and welcome them. hero: 'haze' names the hero and
// team: 2 the team.
onPlayerJoin(({ connection }) => {
  connection.player.seat({ hero: 'any' })
  connection.player.announce({{name}}, 'Welcome, ' + connection.name + '.')
})

// Greet each player who types /hello in chat.
command('hello', (player) => {
  player.chat({{greeting}})
})
`

// scriptConfig is a new script mod's tsconfig.json, which the build's type
// check and editors read. {{scripts}} enables checking JavaScript.
const scriptConfig = `{
  "compilerOptions": {
    "target": "es2023",
    "module": "esnext",
    "moduleResolution": "bundler",
    "lib": ["es2023"],
    "types": [],{{scripts}}
    "jsx": "react-jsx",
    "jsxImportSource": "modlock",
    "strict": true,
    "noEmit": true
  },
  "exclude": ["build", "node_modules"]
}
`

// luauMain is a new Luau mod's entry.
const luauMain = `local modlock = require("@modlock")
local console = require("@modlock/console")

-- Set the server up when the mod starts: no troopers in the lanes or creeps
-- in the jungle, and any number of players on the same hero.
modlock.onStart(function()
	console.apply({
		citadel_trooper_spawn_enabled = false,
		citadel_neutral_spawn_enabled = false,
		citadel_allow_duplicate_heroes = true,
	})
end)

-- Bring a fallen hero back after 3 seconds.
modlock.setRespawnDelay(3)

-- Give each player who joins a hero on the smaller team, skipping the game's
-- team and hero screens, and welcome them. hero = "haze" names the hero and
-- team = 2 the team.
modlock.onPlayerJoin(function(joined)
	local connection = joined.connection
	connection.player:seat({ hero = "any" })
	connection.player:announce({{name}}, "Welcome, " .. connection.name .. ".")
end)

-- Greet each player who types /hello in chat.
modlock.command("hello", function(player)
	player:chat({{greeting}})
end)
`

// pythonMain is a new Python mod's entry.
const pythonMain = `import modlock
from modlock import console


# Set the server up when the mod starts: no troopers in the lanes or creeps
# in the jungle, and any number of players on the same hero.
@modlock.on_start
def start(args: list[str]) -> None:
    console.apply(
        {
            "citadel_trooper_spawn_enabled": False,
            "citadel_neutral_spawn_enabled": False,
            "citadel_allow_duplicate_heroes": True,
        }
    )


# Bring a fallen hero back after 3 seconds.
modlock.set_respawn_delay(3)


# Give each player who joins a hero on the smaller team, skipping the game's
# team and hero screens, and welcome them. hero="haze" names the hero and
# team=2 the team.
@modlock.on_player_join
def joined(event: modlock.PlayerJoinedEvent) -> None:
    connection = event.connection
    connection.player.seat(hero="any")
    connection.player.announce({{name}}, f"Welcome, {connection.name}.")


# Greet each player who types /hello in chat.
@modlock.command("hello")
def hello(player: modlock.Player, args: str) -> None:
    player.chat({{greeting}})
`

// New writes a project for a mod named name in language to dir, which must
// not exist or be empty. A Go project fetches the Modlock library at
// version, such as "v0.2.0" or "latest"; a script, Luau or Python project receives this
// release's library. output receives the tools' messages.
func New(ctx context.Context, dir, name string, language wasm.Manifest_Language, version string, output io.Writer) (*Project, error) {
	// Name the mod after its directory.
	dir, err := filepath.Abs(dir)
	if err != nil {
		return nil, errors.Wrap(err, "create the project")
	}
	if err := Vacant(dir); err != nil {
		return nil, err
	}
	slug := filepath.Base(dir)
	if name == "" {
		name = title(slug)
	}

	// A Go project fetches its library with Go, so find Go before writing
	// anything: a project left half made would block the next attempt.
	if language == wasm.Manifest_LANGUAGE_GO {
		if _, err := exec.LookPath("go"); err != nil {
			return nil, errors.New("a Go mod needs Go: install it from https://go.dev/dl/, open a new terminal and run this again")
		}
	}

	// Choose the language's sources.
	greeting := "Hello from " + name + "!"
	quoted := strings.NewReplacer("{{slug}}", slug, "{{name}}", strconv.Quote(name), "{{greeting}}", strconv.Quote(greeting))
	script := strings.NewReplacer("{{name}}", scriptString(name), "{{greeting}}", scriptString(greeting))
	var files map[string]string
	switch language {
	case wasm.Manifest_LANGUAGE_GO:
		files = map[string]string{
			"go.mod":     "module " + goModule(slug) + "\n\ngo 1.25.0\n",
			"main.go":    quoted.Replace(goMain),
			".gitignore": "/" + BuildDir + "/\n",
		}
	case wasm.Manifest_LANGUAGE_TYPESCRIPT, wasm.Manifest_LANGUAGE_JAVASCRIPT:
		scripts := ""
		if language == wasm.Manifest_LANGUAGE_JAVASCRIPT {
			scripts = "\n    \"allowJs\": true,\n    \"checkJs\": true,"
		}
		files = map[string]string{
			scriptMain(language): script.Replace(scriptMainSource),
			"tsconfig.json":      strings.ReplaceAll(scriptConfig, "{{scripts}}", scripts),
			".gitignore":         "/" + BuildDir + "/\n/node_modules/\n",
		}
	case wasm.Manifest_LANGUAGE_LUAU:
		files = map[string]string{
			"main.luau":  quoted.Replace(luauMain),
			".luaurc":    luauConfig,
			".gitignore": "/" + BuildDir + "/\n/.modlock/\n",
		}
	case wasm.Manifest_LANGUAGE_PYTHON:
		files = map[string]string{
			"main.py":            quoted.Replace(pythonMain),
			"pyrightconfig.json": pythonConfig,
			".gitignore":         "/" + BuildDir + "/\n/.modlock/\n",
		}
	default:
		return nil, errors.Errorf("modlock cannot create %s mods yet", language)
	}
	files[".vscode/extensions.json"] = editorRecommendations(language)

	// Write the manifest and sources.
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, errors.Wrap(err, "create the project")
	}
	manifest := &wasm.Manifest{Slug: slug, Name: name, Version: "0.1.0", Language: language}
	if err := WriteManifest(filepath.Join(dir, ManifestFile), manifest); err != nil {
		return nil, err
	}
	for name, content := range files {
		path := filepath.Join(dir, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return nil, errors.Wrap(err, "create the project")
		}
		if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
			return nil, errors.Wrap(err, "create the project")
		}
	}

	// Install the library.
	created := &Project{Dir: dir, Manifest: manifest}
	switch language {
	case wasm.Manifest_LANGUAGE_LUAU:
		return created, installTree(dir, luau.Library)
	case wasm.Manifest_LANGUAGE_PYTHON:
		return created, installTree(dir, python.Library)
	case wasm.Manifest_LANGUAGE_TYPESCRIPT, wasm.Manifest_LANGUAGE_JAVASCRIPT:
		library, err := js.Library(ctx)
		if err != nil {
			return nil, err
		}
		return created, installLibrary(library, dir)
	}
	for _, arguments := range [][]string{{"get", Module + "@" + version}, {"mod", "tidy"}} {
		cmd := exec.CommandContext(ctx, "go", arguments...)
		cmd.Dir = dir
		cmd.Env = append(os.Environ(), "GOOS=wasip1", "GOARCH=wasm")
		cmd.Stdout = output
		cmd.Stderr = output
		if err := cmd.Run(); err != nil {
			return nil, errors.Wrapf(err, "go %s", strings.Join(arguments, " "))
		}
	}
	return created, nil
}

// Vacant reports whether a project can be created in dir: its last element
// is a valid slug, and it does not exist or is empty. The error says what to
// change.
func Vacant(dir string) error {
	if err := check.Slug(filepath.Base(dir)); err != nil {
		return errors.Wrap(err, "name the directory as the mod's slug")
	}
	if entries, err := os.ReadDir(dir); err == nil && len(entries) > 0 {
		return errors.Errorf("%s already has files in it; choose another name", dir)
	}
	return nil
}

// goModule returns the module path of a new Go mod named slug. Go keeps
// paths without a dot for its standard library and refuses a module named
// after one of its packages, such as go or net, so the path gets a prefix no
// standard package has.
func goModule(slug string) string {
	return "mods/" + slug
}

// SlugFor turns a display name into a slug, such as "Zombie Tag!" into
// "zombie-tag". It returns "" for a name with no ASCII letter or digit.
func SlugFor(name string) string {
	words := strings.FieldsFunc(strings.ToLower(name), func(r rune) bool {
		return (r < 'a' || r > 'z') && (r < '0' || r > '9')
	})
	return strings.Join(words, "-")
}

// scriptString quotes text as a single-quoted JavaScript string.
func scriptString(text string) string {
	return "'" + strings.NewReplacer(`\`, `\\`, `'`, `\'`, "\n", `\n`).Replace(text) + "'"
}

// title turns a slug into a display name, such as "my-mod" into "My Mod".
func title(slug string) string {
	words := strings.FieldsFunc(slug, func(r rune) bool { return r == '-' })
	for i, word := range words {
		words[i] = strings.ToUpper(word[:1]) + word[1:]
	}
	return strings.Join(words, " ")
}
