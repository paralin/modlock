package main

import (
	"testing"

	modcli "github.com/paralin/modlock/cli"
)

func TestCommandLine(t *testing.T) {
	command := func(line string) *modcli.Input {
		return &modcli.Input{Body: &modcli.Input_Command{Command: line}}
	}
	press := func(mod, node, action, payload string) *modcli.Input {
		return &modcli.Input{Body: &modcli.Input_Press{Press: &modcli.Press{
			Mod: mod, Node: node, Action: action, Payload: payload,
		}}}
	}
	cases := []struct {
		in   *modcli.Input
		want string
	}{
		{command("monopolis_requestdiceroll"), "monopolis_requestdiceroll"},
		{command("/start"), "start"},
		{command("  /start  "), "start"},
		{command(""), ""},
		{press("arena", "card", "", ""), "modlockpress arena card"},
		{press("arena", "card", "close", ""), "modlockaction arena card close"},
		{press("arena", "card", "vote", `{"vote":1}`), `modlockaction arena card vote {"vote":1}`},
	}
	for _, c := range cases {
		if got := commandLine(c.in); got != c.want {
			t.Errorf("commandLine() = %q, want %q", got, c.want)
		}
	}
}
