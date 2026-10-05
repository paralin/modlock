// Package session runs a play session: modlock-host with the mods, a Deadlock
// client that joins it, and the control link that reports the mods' progress
// and reloads them.
package session

import (
	"bufio"
	"context"
	"encoding/binary"
	"io"
	"net"

	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/pkg/errors"
)

// maxMessage bounds one framed message from the host.
const maxMessage = 10_000_000

// Control is the controller's end of the control link. It listens on a
// loopback port and modlock-host dials it with --control; then the host
// streams its events and takes reloads, each message framed by
// its length as control.proto describes.
type Control struct {
	// listener accepts the host's connection.
	listener net.Listener
	// requests carries requests to the host once it connects.
	requests chan *control.ControlRequest
}

// NewControl listens on a free loopback port.
func NewControl() (*Control, error) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, errors.Wrap(err, "listen for the host")
	}
	return &Control{listener: listener, requests: make(chan *control.ControlRequest, 16)}, nil
}

// Address returns the address to pass to modlock-host --control.
func (c *Control) Address() string {
	return c.listener.Addr().String()
}

// Reload asks the host to replace the running mod named by the build at path.
// Requests made before the host connects wait for it.
func (c *Control) Reload(ctx context.Context, path string) error {
	return c.request(ctx, &control.ControlRequest{
		Body: &control.ControlRequest_Reload{Reload: &control.ReloadRequest{Path: path}},
	})
}

// request queues request for the call.
func (c *Control) request(ctx context.Context, request *control.ControlRequest) error {
	select {
	case c.requests <- request:
		return nil
	case <-ctx.Done():
		return context.Canceled
	}
}

// Run accepts the host's connection and passes each host event to handle
// until the host disconnects or ctx ends. It returns nil when the host
// disconnects.
func (c *Control) Run(ctx context.Context, handle func(*control.HostEvent)) error {
	// Accept one host; closing the listener ends the wait with ctx.
	stop := context.AfterFunc(ctx, func() { _ = c.listener.Close() })
	defer stop()
	conn, err := c.listener.Accept()
	if err != nil {
		if ctx.Err() != nil {
			return context.Canceled
		}
		return errors.Wrap(err, "accept the host")
	}
	defer conn.Close()

	// Forward requests while the host is connected.
	sendCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	go c.send(sendCtx, conn)

	// Pass events until the host disconnects.
	stopRead := context.AfterFunc(ctx, func() { _ = conn.Close() })
	defer stopRead()
	reader := bufio.NewReader(conn)
	for {
		event := &control.HostEvent{}
		if err := readMessage(reader, event); err != nil {
			if ctx.Err() != nil {
				return context.Canceled
			}
			return nil
		}
		handle(event)
	}
}

// send writes requests to conn until ctx ends or a write fails.
func (c *Control) send(ctx context.Context, conn net.Conn) {
	for {
		select {
		case <-ctx.Done():
			return
		case request := <-c.requests:
			data, err := request.MarshalVT()
			if err != nil {
				continue
			}
			frame := binary.LittleEndian.AppendUint32(make([]byte, 0, 4+len(data)), uint32(len(data)))
			if _, err := conn.Write(append(frame, data...)); err != nil {
				return
			}
		}
	}
}

// readMessage reads one framed message into message.
func readMessage(reader io.Reader, message interface{ UnmarshalVT([]byte) error }) error {
	var header [4]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return err
	}
	size := binary.LittleEndian.Uint32(header[:])
	if size > maxMessage {
		return errors.Errorf("the host sent a %d byte message", size)
	}
	data := make([]byte, size)
	if _, err := io.ReadFull(reader, data); err != nil {
		return err
	}
	return message.UnmarshalVT(data)
}

// Close stops listening for the host.
func (c *Control) Close() error {
	return c.listener.Close()
}
