// Package console sets the server's console variables and runs its console
// commands, through values modlock-entitygen generates from the game's
// console:
//
//	err := console.SvCheats.Set(true)
//	err = console.Changelevel.Run("street_test")
//
// Each runs one line at the server console through mod.ServerCommand, which
// may set a development-only or cheat-protected variable. A variable of a
// type other than a scalar or a string, such as a vector, has no value here;
// set it with mod.ServerCommand.
package console
