package main

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"strings"
)

// partsFile assigns each Host call to the part of the host that answers it.
const partsFile = "src/wasm/host_calls.txt"

// part is one part of the host: the Host calls one object answers.
type part struct {
	// name is the part's name in the file, such as heroes.
	name string
	// methods are its calls in the order the Host service declares them.
	methods []*method
}

// loadParts validates the Host partition before retaining it for rendering.
func (s *schema) loadParts(path string) error {
	// Read and validate a complete partition before replacing the current one.
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	parts, err := parseParts(data, s.host)
	if err != nil {
		return err
	}
	s.parts = parts
	return nil
}

// parseParts parses a parts file: lines of a part name, a colon and its
// calls, where an indented line continues the part above it and a line that
// starts with # is a comment.
func parseParts(data []byte, host *service) ([]*part, error) {
	// Index the Host methods before assigning each one to a part.
	methods := map[string]*method{}
	for _, m := range host.methods {
		methods[m.name] = m
	}
	owner := map[string]*part{}
	var parts []*part
	var current *part
	scanner := bufio.NewScanner(bytes.NewReader(data))
	for n := 1; scanner.Scan(); n++ {
		line := scanner.Text()
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}
		names := trimmed
		if line[0] != ' ' && line[0] != '\t' {
			name, rest, ok := strings.Cut(trimmed, ":")
			if !ok || !isPartName(name) {
				return nil, fmt.Errorf("%s:%d: want a part name, a colon and its calls", partsFile, n)
			}
			for _, p := range parts {
				if p.name == name {
					return nil, fmt.Errorf("%s:%d: part %s is listed twice", partsFile, n, name)
				}
			}
			current = &part{name: name}
			parts = append(parts, current)
			names = rest
		} else if current == nil {
			return nil, fmt.Errorf("%s:%d: a continuation line needs a part above it", partsFile, n)
		}
		for name := range strings.FieldsSeq(names) {
			m := methods[name]
			if m == nil {
				return nil, fmt.Errorf("%s:%d: Host has no call %s", partsFile, n, name)
			}
			if p := owner[name]; p != nil {
				return nil, fmt.Errorf("%s:%d: %s is already in part %s", partsFile, n, name, p.name)
			}
			owner[name] = current
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}

	// Keep each part's calls in the service's order, so the generated
	// interfaces read as the schema does.
	for _, m := range host.methods {
		p := owner[m.name]
		if p == nil {
			return nil, fmt.Errorf("%s: Host call %s is in no part", partsFile, m.name)
		}
		p.methods = append(p.methods, m)
	}
	return parts, nil
}

// isPartName reports whether name is a lower-case identifier, which names a
// field of HostCalls and, capitalized, its interface.
func isPartName(name string) bool {
	if name == "" || name[0] < 'a' || name[0] > 'z' {
		return false
	}
	for _, r := range name {
		if (r < 'a' || r > 'z') && (r < '0' || r > '9') {
			return false
		}
	}
	return true
}

// partInterface returns the C++ interface of a part, such as HeroesCalls.
func partInterface(p *part) string {
	return strings.ToUpper(p.name[:1]) + p.name[1:] + "Calls"
}
