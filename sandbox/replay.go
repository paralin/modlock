package sandbox

import (
	"bufio"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"time"

	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/trace"
)

// maxRecord bounds one trace record, far above any event or call the host
// sends, so a damaged length cannot ask for gigabytes.
const maxRecord = 64 << 20

// Replay plays a trace the host recorded of a real session to both builds:
// each event in order, at the time the host delivered it, with each of the
// reference's host calls answered by the reply the host gave the first equal
// call it recorded after that event, and an empty reply when the host
// recorded none. Start carries source in place of the script the traced
// build ran, so a script reference runs the script under test. Replay reads
// one event and its calls at a time, stops at the first divergence and
// returns it, and otherwise ignores the reference's own errors, as a session
// does. A trace cut short by its size limit replays up to where it ends.
func (t *Twin) Replay(ctx context.Context, traced io.Reader, source []byte) error {
	// Skip the calls the traced build made while it initialized, which this
	// pair already made at load.
	reader := bufio.NewReader(traced)
	record, err := readRecord(reader)
	for err == nil && record.GetEvent() == nil {
		record, err = readRecord(reader)
	}

	// Answer each event's calls from its recorded ones.
	var calls []twinCall
	var taken []bool
	t.answer = func(call *wasm.Call) *wasm.Reply {
		for i, recorded := range calls {
			if !taken[i] && sameCall(recorded.call, call) {
				taken[i] = true
				return recorded.reply
			}
		}
		return &wasm.Reply{}
	}
	for err == nil && !t.Stopped() {
		// Read the event's calls, up to the next event.
		event := record.GetEvent()
		calls, taken = calls[:0], taken[:0]
		for record, err = readRecord(reader); err == nil && record.GetEvent() == nil; record, err = readRecord(reader) {
			calls = append(calls, twinCall{call: record.GetCall().GetCall(), reply: record.GetCall().GetReply()})
			taken = append(taken, false)
		}

		// Move the clock to the event, never back, and deliver it.
		if at := time.Unix(0, event.GetUnixNanos()); at.After(t.now) {
			t.now = at
		}
		method, request := event.GetCall().GetMethod(), raw(event.GetCall().GetRequest())
		if method == "Start" {
			started, startErr := startWith(request, source)
			if startErr != nil {
				return startErr
			}
			request = started
		}
		if deliverErr := t.Deliver(ctx, method, &request, nil); diverged(deliverErr) {
			return deliverErr
		}
	}
	if errors.Is(err, io.EOF) {
		return nil
	}
	return err
}

// readRecord reads one length-delimited record. It returns io.EOF at the end
// of the trace, including within a record the size limit cut short.
func readRecord(reader *bufio.Reader) (*trace.Record, error) {
	size, err := binary.ReadUvarint(reader)
	if err != nil {
		if errors.Is(err, io.ErrUnexpectedEOF) {
			return nil, io.EOF
		}
		return nil, err
	}
	if size > maxRecord {
		return nil, fmt.Errorf("a trace record of %d bytes is past the %d byte limit", size, maxRecord)
	}
	data := make([]byte, size)
	if _, err := io.ReadFull(reader, data); err != nil {
		return nil, io.EOF
	}
	record := &trace.Record{}
	if err := record.UnmarshalVT(data); err != nil {
		return nil, fmt.Errorf("read a trace record: %w", err)
	}
	return record, nil
}

// startWith returns the Start request with source as its script.
func startWith(request raw, source []byte) (raw, error) {
	start := &wasm.StartEvent{}
	if err := start.UnmarshalVT(request); err != nil {
		return nil, fmt.Errorf("read the traced Start: %w", err)
	}
	start.Source = source
	encoded, err := start.MarshalVT()
	return encoded, err
}
