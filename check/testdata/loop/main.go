// Command loop never finishes initializing.
package main

import _ "github.com/paralin/modlock/mod"

// spin counts forever, so the compiler keeps the loop.
var spin int

// init never returns.
func init() {
	for {
		spin++
	}
}

// main never runs.
func main() {}
