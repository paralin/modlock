package main

import (
	"unsafe"

	"golang.org/x/sys/windows"
)

// getConsoleProcessList is the Windows call that lists the processes
// attached to this process's console.
var getConsoleProcessList = windows.NewLazySystemDLL("kernel32.dll").NewProc("GetConsoleProcessList")

// ownConsole reports whether this process is the only one attached to its
// console. That is the case when a person opens the program from Explorer:
// Windows makes a console window for it, which closes when the program ends.
// A program started from a terminal shares the console with its shell.
func ownConsole() bool {
	var processes [2]uint32
	count, _, _ := getConsoleProcessList.Call(uintptr(unsafe.Pointer(&processes[0])), uintptr(len(processes)))
	return count == 1
}
