package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/internal/fetch"
	"github.com/paralin/modlock/internal/prompt"
	"github.com/paralin/modlock/internal/selfupdate"
	"github.com/paralin/modlock/project"
	"github.com/pkg/errors"
)

// marketplace is the Modlock extension's page in the Visual Studio
// Marketplace, for a person to install it by hand.
const marketplace = "https://marketplace.visualstudio.com/items?itemName=" + project.Extension

// editor is one editor built on VS Code that takes the Modlock extension.
type editor struct {
	// name is the editor as a person reads it.
	name string
	// command is the editor's command line on PATH.
	command string
	// home is the directory under the home directory that keeps the
	// editor's extensions, in its extensions directory.
	home string
	// marker is lowercase text that the paths of the editor's own install
	// contain, which tells its built-in terminal apart from the others'.
	marker string
}

// editors lists the editors modlock sets up, VS Code first. Its forks' paths
// may also contain VS Code's marker, so a terminal is matched against them
// before VS Code.
var editors = []editor{
	{name: "VS Code", command: "code", home: ".vscode", marker: "code"},
	{name: "VS Code Insiders", command: "code-insiders", home: ".vscode-insiders", marker: "insiders"},
	{name: "Cursor", command: "cursor", home: ".cursor", marker: "cursor"},
	{name: "Windsurf", command: "windsurf", home: ".windsurf", marker: "windsurf"},
	{name: "VSCodium", command: "codium", home: ".vscode-oss", marker: "codium"},
}

// localEditor is an editor on this computer with the program that runs its
// command line.
type localEditor struct {
	editor
	// program is the path of the editor's command line.
	program string
}

// editorCommand installs the Modlock extension, and in a mod project the
// language's extension, into the editor in use.
func editorCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "editor",
		Usage: "set up your editor for Modlock mods",
		Flags: []cli.Flag{
			&cli.StringFlag{Name: "editor", Usage: "the editor to set up: code, code-insiders, cursor, windsurf or codium (default: the editor whose terminal this is, or every one installed)"},
		},
		Action: func(c *cli.Context) error {
			// Choose the editors.
			chosen, err := chooseEditors(c.String("editor"))
			if err != nil {
				return err
			}

			// Find the extension for the language of the project here, if
			// this is one.
			var languageID, languageLabel string
			if opened, err := project.Open("."); err == nil {
				languageID = project.LanguageExtension(opened.Manifest.GetLanguage())
				if at := slices.IndexFunc(languages, func(l language) bool { return l.manifest == opened.Manifest.GetLanguage() }); at >= 0 {
					languageLabel = languages[at].label
				}
			}

			// Set each editor up, carrying on past one that fails.
			failed := false
			for _, e := range chosen {
				if err := setUpEditor(c.Context, out, e); err != nil {
					out.mark("✗", red, err.Error())
					failed = true
					continue
				}
				if languageID != "" {
					addLanguage(c.Context, out, e, languageID, languageLabel)
				}
			}
			if failed {
				return cli.Exit("", 1)
			}
			return nil
		},
	}
}

// chooseEditors returns the editors to set up: the one name gives, by command
// or name, or else the one whose built-in terminal runs modlock, or else every
// one whose command line is on PATH.
func chooseEditors(name string) ([]localEditor, error) {
	// Take the editor the flag names.
	inside, insideOK := terminalEditor()
	if name != "" {
		at := slices.IndexFunc(editors, func(e editor) bool {
			return strings.EqualFold(name, e.command) || strings.EqualFold(name, e.name) || strings.EqualFold(name, strings.ReplaceAll(e.name, " ", ""))
		})
		if at < 0 {
			return nil, errors.Errorf("modlock does not know the editor %q; choose code, code-insiders, cursor, windsurf or codium", name)
		}
		if insideOK && inside.command == editors[at].command {
			return []localEditor{inside}, nil
		}
		program, err := exec.LookPath(editors[at].command)
		if err != nil {
			return nil, errors.Errorf("modlock cannot find %s's command line, %s; install it from the editor's command palette, or install the Modlock extension from %s", editors[at].name, editors[at].command, marketplace)
		}
		return []localEditor{{editor: editors[at], program: program}}, nil
	}

	// Take the editor whose terminal this is.
	if insideOK {
		return []localEditor{inside}, nil
	}

	// Take every editor installed.
	var found []localEditor
	for _, e := range editors {
		if program, err := exec.LookPath(e.command); err == nil {
			found = append(found, localEditor{editor: e, program: program})
		}
	}
	if len(found) == 0 {
		return nil, errors.Errorf("modlock found none of VS Code, VS Code Insiders, Cursor, Windsurf or VSCodium; install one, or install the Modlock extension from %s", marketplace)
	}
	return found, nil
}

// preferredEditor returns the editor modlock new offers: the one whose
// terminal this is, or the first one installed. It returns nil when there is
// none.
func preferredEditor() *localEditor {
	found, err := chooseEditors("")
	if err != nil {
		return nil
	}
	return &found[0]
}

// terminalEditor returns the editor whose built-in terminal runs modlock, and
// false outside one, in an editor modlock does not know, or when the editor's
// command line cannot be found. Every one of these editors sets TERM_PROGRAM
// to vscode in its terminal, and its Git integration sets the paths of its own
// install in VSCODE_GIT_ASKPASS_NODE and VSCODE_GIT_ASKPASS_MAIN, which name
// the fork.
func terminalEditor() (localEditor, bool) {
	// Only an editor's terminal says it is one.
	if os.Getenv("TERM_PROGRAM") != "vscode" {
		return localEditor{}, false
	}

	// Name the editor from its install's paths, taking VS Code when the
	// terminal leaves them out.
	askpass := os.Getenv("VSCODE_GIT_ASKPASS_MAIN")
	paths := strings.ToLower(os.Getenv("VSCODE_GIT_ASKPASS_NODE") + "\n" + askpass)
	at := 0
	if strings.TrimSpace(paths) != "" {
		at = slices.IndexFunc(editors[1:], func(e editor) bool { return strings.Contains(paths, e.marker) }) + 1
		if at == 0 && !strings.Contains(paths, editors[0].marker) {
			return localEditor{}, false
		}
	}
	chosen := editors[at]

	// Run its command line from PATH, or from its install when the person
	// never put it on PATH, as on a Mac.
	if program, err := exec.LookPath(chosen.command); err == nil {
		return localEditor{editor: chosen, program: program}, true
	}
	if program := bundledProgram(askpass, chosen.command); program != "" {
		return localEditor{editor: chosen, program: program}, true
	}
	return localEditor{}, false
}

// bundledProgram returns the command line inside the editor install that
// holds askpass, the Git integration's script at
// <app>/extensions/git/dist/askpass-main.js, or "" when it has none. A Mac
// keeps the command line at <app>/bin, and Windows and Linux two directories
// above app, under bin; a fork may name it code rather than command.
func bundledProgram(askpass, command string) string {
	// Find the install's app directory.
	if askpass == "" {
		return ""
	}
	app := filepath.Dir(filepath.Dir(filepath.Dir(filepath.Dir(askpass))))

	// Take the first command line there, with the extension Windows needs.
	for _, dir := range []string{filepath.Join(app, "bin"), filepath.Join(app, "..", "..", "bin")} {
		for _, name := range []string{command, "code"} {
			if program, err := exec.LookPath(filepath.Join(dir, name)); err == nil {
				return program
			}
		}
	}
	return ""
}

// has reports whether the editor's extensions directory holds the extension
// id, in a directory named for the id and its version.
func (e localEditor) has(id string) bool {
	home, err := os.UserHomeDir()
	if err != nil {
		return false
	}
	matches, _ := filepath.Glob(filepath.Join(home, e.home, "extensions", strings.ToLower(id)+"-*"))
	return len(matches) != 0
}

// install runs the editor's command line to install extension, an identifier
// in its store or the path of a .vsix file. The error carries the command
// line's last line of output.
func (e localEditor) install(ctx context.Context, extension string) error {
	// Run the command line.
	output, err := exec.CommandContext(ctx, e.program, "--install-extension", extension).CombinedOutput()
	if err == nil {
		return nil
	}

	// Explain the failure with its last line of output.
	lines := strings.Split(strings.TrimSpace(string(output)), "\n")
	if last := strings.TrimSuffix(strings.TrimSpace(lines[len(lines)-1]), "."); last != "" {
		return errors.New(last)
	}
	return err
}

// setUpEditor installs the Modlock extension into e unless it is there: from
// the editor's extension store, or else from this release's .vsix file. It
// returns an error, with the link to install it by hand, when both fail.
func setUpEditor(ctx context.Context, out *printer, e localEditor) error {
	// Leave an extension already installed.
	if e.has(project.Extension) {
		out.mark("✓", green, "the Modlock extension is already in "+e.name)
		return nil
	}

	// Install it from the editor's store.
	out.mark("…", dim, "installing the Modlock extension into "+e.name)
	stored := e.install(ctx, project.Extension)
	if stored == nil {
		out.mark("✓", green, "installed the Modlock extension into "+e.name+" from its extension store")
		return nil
	}

	// Install the release's own file instead.
	url := extensionFile()
	if err := installFile(ctx, e, url); err != nil {
		return errors.Errorf("%s could not install the Modlock extension from its store (%v) or from %s (%v); install it from %s", e.name, stored, url, err, marketplace)
	}
	out.mark("✓", green, "installed the Modlock extension into "+e.name+" from "+url)
	return nil
}

// extensionFile returns the address of the Modlock extension's .vsix file in
// this release, or in the newest one for a development build.
func extensionFile() string {
	if !selfupdate.Released(Version) {
		return "https://github.com/paralin/modlock/releases/latest/download/modlock.vsix"
	}
	return fetch.Release(Version, "modlock.vsix")
}

// installFile downloads the .vsix file at url to a temporary directory and
// installs it into e, then removes it.
func installFile(ctx context.Context, e localEditor, url string) error {
	// Download the file.
	dir, err := os.MkdirTemp("", "modlock-editor-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "modlock.vsix")
	if err := fetch.File(ctx, url, path); err != nil {
		return err
	}

	// Install it.
	return e.install(ctx, path)
}

// addLanguage installs id, the extension that reads label's sources, into e
// unless it is there. Its failure only leaves a note: the project's
// .vscode/extensions.json has the editor offer it again.
func addLanguage(ctx context.Context, out *printer, e localEditor, id, label string) {
	// Leave an extension already installed.
	if e.has(id) {
		out.mark("✓", green, "the "+label+" extension, "+id+", is already in "+e.name)
		return
	}

	// Install it from the editor's store.
	if err := e.install(ctx, id); err != nil {
		out.mark("!", yellow, e.name+" could not install the "+label+" extension, "+id+": "+err.Error()+"; the editor offers it again when it opens the project")
		return
	}
	out.mark("✓", green, "installed the "+label+" extension, "+id+", into "+e.name)
}

// offerExtension asks, through ask, whether to install the Modlock extension
// into e, and installs it on yes. A failed install leaves a warning, since the
// command that offered it goes on without it; only a cancelled question
// returns an error.
func offerExtension(ctx context.Context, out *printer, ask *prompt.Terminal, e localEditor) error {
	// Ask.
	yes, err := ask.Confirm("Install the Modlock extension for "+e.name+"?", true)
	if err != nil || !yes {
		return err
	}

	// Install it, warning on failure.
	if err := setUpEditor(ctx, out, e); err != nil {
		out.mark("!", yellow, err.Error())
	}
	return nil
}

// offerExtensionOnce offers the Modlock extension to the editor whose
// built-in terminal runs this session, when it lacks it, once ever: the
// settings remember the offer whatever the answer. It asks only a person at a
// terminal, never under --json or CI. Only a cancelled question returns an
// error.
func offerExtensionOnce(ctx context.Context, out *printer) error {
	// Ask only a person in an editor's terminal, in an editor without the
	// extension.
	ask := asker(out)
	if ask == nil || os.Getenv("CI") != "" {
		return nil
	}
	e, ok := terminalEditor()
	if !ok || e.has(project.Extension) {
		return nil
	}

	// Ask once: remember the offer before making it, and skip it when the
	// settings cannot remember it, so it never repeats.
	config, path, err := readConfig()
	if err != nil || config.GetEditorOffered() {
		return nil
	}
	config.EditorOffered = true
	if err := writeConfig(path, config); err != nil {
		return nil
	}
	return offerExtension(ctx, out, ask, e)
}
