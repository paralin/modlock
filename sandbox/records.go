package sandbox

import (
	"github.com/paralin/modlock/records"
	"github.com/paralin/modlock/src/wasm"
	recordspb "github.com/paralin/modlock/src/wasm/records"
)

// records answers the records service as a host without a backend: it
// checks each record against the kinds manifest declares and logs it as it
// would be stored, naming any field it left out. A record of an undeclared
// kind fails, so the mod's author sees why.
func (s *Sandbox) records(manifest *wasm.Manifest, call *wasm.ServiceCall) *wasm.Reply {
	// Decode the one method's request.
	mod := manifest.GetSlug()
	if call.GetMethod() != "Add" {
		return &wasm.Reply{Error: "the records service has no method " + call.GetMethod()}
	}
	record := &recordspb.GameRecord{}
	if err := record.UnmarshalVT(call.GetPayload()); err != nil {
		return &wasm.Reply{Error: "the records Add request is malformed"}
	}

	// Check it against its kind and log what a host would store.
	checked, dropped, err := records.Check(manifest, record)
	if err != nil {
		return s.refuse(mod, err.Error())
	}
	line := "record " + records.Describe(checked)
	if len(dropped) != 0 {
		line += "; left out " + records.DescribeDropped(dropped)
	}
	s.log(mod, line)
	reply, _ := (&wasm.ServiceReply{}).MarshalVT()
	return &wasm.Reply{Response: reply}
}
