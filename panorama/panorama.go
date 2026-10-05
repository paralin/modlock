// Package panorama holds the renderer that draws mods' interfaces in the
// player's game. A host whose own interface script receives the screens may
// compile Drawing from src/draw.ts into that script; vendoring this package
// delivers the source beside the ui protocol it reads.
package panorama

import "embed"

// Sources holds the drawing's TypeScript source.
//
//go:embed src/draw.ts
var Sources embed.FS
