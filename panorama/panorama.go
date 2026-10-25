// Package panorama holds the renderer that draws mods' interfaces in the
// player's game. A host whose own interface script receives the screens may
// compile Drawing from src/draw.ts into that script, and Hud from src/hud.ts
// to hide the parts of the game's HUD the screens list; vendoring this package
// delivers the source beside the ui protocol it reads. A page may draw the
// same screens in a browser on browser/dom.ts, the stand-in for game panels,
// with the layout rules in browser/panels.css.
package panorama

import "embed"

// Sources holds the drawing's and the hud's TypeScript source and the
// drawing's browser stand-in.
//
//go:embed src/draw.ts src/data.ts src/hud.ts browser/dom.ts browser/panels.css
var Sources embed.FS

// Layout is the renderer's layout, which a client content package installs
// as panorama/layout/modlock/ui.xml beside the built renderer script.
//
//go:embed layout/modlock/ui.xml
var Layout []byte
