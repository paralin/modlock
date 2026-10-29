package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/aperturerobotics/cli"
	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/panorama"
	"github.com/paralin/modlock/source2/resource"
	"github.com/paralin/modlock/source2/vpk"
	"github.com/pkg/errors"
)

// archiveFlag reads an input from inside an archive, such as the game's
// pak01_dir.vpk, instead of from a file.
var archiveFlag = &cli.StringFlag{Name: "archive", Usage: "read `PATH` from inside this archive, such as Deadlock's game/citadel/pak01_dir.vpk"}

// panoramaCommand compiles and decompiles Panorama interface files.
func panoramaCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "panorama",
		Usage: "compile and decompile Panorama layouts, scripts and style sheets without Valve's tools",
		Subcommands: []*cli.Command{
			{
				Name:      "compile",
				Usage:     "compile an addon's content directory into its game files",
				ArgsUsage: "DIRECTORY",
				Description: "DIRECTORY holds the addon's sources as the game names them, such as\n" +
					"panorama/layout/menu.xml, panorama/scripts/menu.js and panorama/styles/menu.css.\n" +
					"Each layout, script and style sheet compiles to its .vxml_c, .vjs_c or .vcss_c\n" +
					"resource; other files, such as images already compiled, are kept. The result is\n" +
					"written to --output: an archive when it ends in .vpk, otherwise a directory.",
				Flags: []cli.Flag{
					&cli.StringFlag{Name: "output", Aliases: []string{"o"}, Usage: "the archive or directory to write (default: DIRECTORY.vpk)"},
					&cli.StringFlag{Name: "addon", Usage: "the addon name the resources record (default: DIRECTORY's name)"},
				},
				Action: func(c *cli.Context) error {
					// Compile the directory.
					if c.NArg() != 1 {
						return errors.New("name the addon's content directory, such as: modlock panorama compile content")
					}
					dir := filepath.Clean(c.Args().First())
					addon := c.String("addon")
					if addon == "" {
						addon = filepath.Base(dir)
					}
					files, err := panorama.CompileTree(addon, os.DirFS(dir))
					if err != nil {
						return err
					}

					// Write the archive or the directory.
					output := c.String("output")
					if output == "" {
						output = dir + ".vpk"
					}
					if err := writeTree(output, files); err != nil {
						return err
					}
					out.note("compiled", len(files), "files into", output)
					return nil
				},
			},
			{
				Name:      "decompile",
				Usage:     "print a compiled layout as XML, a compiled style sheet as CSS, or a compiled script as its source",
				ArgsUsage: "FILE",
				Description: "FILE is a .vxml_c, .vcss_c or .vjs_c resource. With --archive, FILE names it inside the\n" +
					"archive, as in: modlock panorama decompile --archive pak01_dir.vpk panorama/layout/hud.vxml_c",
				Flags: []cli.Flag{archiveFlag},
				Action: func(c *cli.Context) error {
					// Read the resource.
					data, err := readInput(c)
					if err != nil {
						return err
					}

					// A script stores its source; a style sheet prints from its
					// text and a layout from its tree.
					var text string
					switch path.Ext(c.Args().First()) {
					case ".vjs_c":
						source, err := resource.ReadBlock(data, "DATA")
						if err != nil {
							return err
						}
						_, err = os.Stdout.Write(source)
						return err
					case ".vcss_c":
						text, err = panorama.DecompileStyle(data)
					default:
						text, err = panorama.Decompile(data)
					}
					if err != nil {
						return err
					}
					_, err = os.Stdout.WriteString(text)
					return err
				},
			},
		},
	}
}

// vpkCommand packs, lists and unpacks Valve archives.
func vpkCommand(out *printer) *cli.Command {
	return &cli.Command{
		Name:  "vpk",
		Usage: "pack, list and unpack Valve archives",
		Subcommands: []*cli.Command{
			{
				Name:      "pack",
				Usage:     "pack a directory into an archive the game mounts",
				ArgsUsage: "DIRECTORY ARCHIVE",
				Description: "Equal directories pack to equal bytes: files are ordered by extension,\n" +
					"directory and name, and stored in the archive's own file.",
				Action: func(c *cli.Context) error {
					if c.NArg() != 2 {
						return errors.New("name the directory and the archive, such as: modlock vpk pack game pak01_dir.vpk")
					}
					if err := writeArchive(c.Args().Get(1), os.DirFS(c.Args().First())); err != nil {
						return err
					}
					out.note("packed", c.Args().Get(1))
					return nil
				},
			},
			{
				Name:      "list",
				Usage:     "list an archive's files and their sizes",
				ArgsUsage: "ARCHIVE",
				Action: func(c *cli.Context) error {
					archive, err := openArchive(c)
					if err != nil {
						return err
					}
					for _, name := range archive.Names() {
						size, _ := archive.Size(name)
						fmt.Printf("%10d  %s\n", size, name)
					}
					return nil
				},
			},
			{
				Name:      "unpack",
				Usage:     "write an archive's files, or the files under the paths named, into a directory",
				ArgsUsage: "ARCHIVE DIRECTORY [PATH...]",
				Description: "ARCHIVE may be a directory of numbered parts, such as Deadlock's\n" +
					"game/citadel/pak01_dir.vpk. Each PATH names a file or a directory inside it,\n" +
					"such as panorama/layout.",
				Action: func(c *cli.Context) error {
					// Open the archive and choose its files.
					if c.NArg() < 2 {
						return errors.New("name the archive and a directory, such as: modlock vpk unpack pak01_dir.vpk out panorama/layout")
					}
					archive, err := openArchive(c)
					if err != nil {
						return err
					}
					paths := c.Args().Slice()[2:]
					var chosen []string
					for _, name := range archive.Names() {
						if len(paths) == 0 || underAny(name, paths) {
							chosen = append(chosen, name)
						}
					}
					if len(chosen) == 0 {
						return errors.Errorf("%s holds nothing under %s", c.Args().First(), strings.Join(paths, ", "))
					}

					// Write each one.
					dir := c.Args().Get(1)
					for _, name := range chosen {
						data, err := archive.ReadFile(name)
						if err != nil {
							return err
						}
						if err := writeFile(filepath.Join(dir, filepath.FromSlash(name)), data); err != nil {
							return err
						}
					}
					out.note("unpacked", len(chosen), "files into", dir)
					return nil
				},
			},
		},
	}
}

// kv3Command prints a compiled resource's KeyValues3 data as JSON.
func kv3Command() *cli.Command {
	return &cli.Command{
		Name:      "kv3",
		Usage:     "print the KeyValues3 data of a compiled resource as JSON",
		ArgsUsage: "FILE",
		Description: "FILE is a compiled resource, such as a .vxml_c layout or a .vdata_c file, or bare\n" +
			"binary KeyValues3. Without --block, the first block holding KeyValues3 prints,\n" +
			"ahead of RED2, the compiler's record of its inputs; --blocks lists them all.",
		Flags: []cli.Flag{
			archiveFlag,
			&cli.StringFlag{Name: "block", Usage: "the block to print, such as DATA, LaCo or RED2"},
			&cli.BoolFlag{Name: "blocks", Usage: "list the resource's blocks instead"},
		},
		Action: func(c *cli.Context) error {
			// Read the input; bare KeyValues3 prints as it is.
			data, err := readInput(c)
			if err != nil {
				return err
			}
			value, err := kv3.Decode(data)
			if err == nil {
				return printJSON(value)
			}

			// List the blocks, or print the one asked for.
			blocks, err := resource.Read(data)
			if err != nil {
				return err
			}
			if c.Bool("blocks") {
				for _, block := range blocks {
					fmt.Printf("%s %8d\n", block.Tag, len(block.Data))
				}
				return nil
			}
			if tag := c.String("block"); tag != "" {
				data, ok := resource.Find(blocks, tag)
				if !ok {
					return errors.Errorf("the resource has no %s block; list them with --blocks", tag)
				}
				return printKV3(data)
			}
			for _, block := range blocks {
				if block.Tag == "RED2" {
					continue
				}
				if value, err := kv3.Decode(block.Data); err == nil {
					return printJSON(value)
				}
			}
			if data, ok := resource.Find(blocks, "RED2"); ok {
				return printKV3(data)
			}
			return errors.New("no block of the resource holds KeyValues3; list them with --blocks")
		},
	}
}

// readInput reads the file named by the first argument, from inside the
// --archive when one is named.
func readInput(c *cli.Context) ([]byte, error) {
	if c.NArg() != 1 {
		return nil, errors.New("name one file")
	}
	if c.String("archive") == "" {
		return os.ReadFile(c.Args().First())
	}
	archive, err := vpk.OpenArchive(c.String("archive"))
	if err != nil {
		return nil, err
	}
	return archive.ReadFile(c.Args().First())
}

// openArchive opens the archive named by the first argument.
func openArchive(c *cli.Context) (*vpk.Archive, error) {
	if c.NArg() < 1 {
		return nil, errors.New("name the archive")
	}
	return vpk.OpenArchive(c.Args().First())
}

// underAny reports whether name is one of paths or inside one of them.
func underAny(name string, paths []string) bool {
	for _, p := range paths {
		p = strings.Trim(strings.ToLower(filepath.ToSlash(p)), "/")
		if name == p || strings.HasPrefix(name, p+"/") {
			return true
		}
	}
	return false
}

// writeTree writes files as an archive when output ends in .vpk, otherwise
// into the directory output.
func writeTree(output string, files fs.FS) error {
	if strings.HasSuffix(output, ".vpk") {
		return writeArchive(output, files)
	}
	return fs.WalkDir(files, ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return err
		}
		data, err := fs.ReadFile(files, name)
		if err != nil {
			return err
		}
		return writeFile(filepath.Join(output, filepath.FromSlash(name)), data)
	})
}

// writeArchive packs files into the archive at path.
func writeArchive(path string, files fs.FS) error {
	var packed bytes.Buffer
	if err := vpk.Write(&packed, files); err != nil {
		return err
	}
	return writeFile(path, packed.Bytes())
}

// writeFile writes data to path, creating its directory.
func writeFile(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o644)
}

// printKV3 prints binary KeyValues3 as JSON.
func printKV3(data []byte) error {
	value, err := kv3.Decode(data)
	if err != nil {
		return err
	}
	return printJSON(value)
}

// printJSON prints value as indented JSON.
func printJSON(value any) error {
	encoder := json.NewEncoder(os.Stdout)
	encoder.SetIndent("", "  ")
	return encoder.Encode(value)
}
