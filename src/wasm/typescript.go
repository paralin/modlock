package wasm

import "embed"

// typescript keeps the library's generated messages when vendoring, including
// messages that the library imports from child packages only in TypeScript.
//
//go:embed */*.pb.ts
var typescript embed.FS
