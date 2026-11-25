package texture

import (
	"encoding/binary"
	"math"
	"testing"

	"github.com/paralin/modlock/source2/resource"
)

// TestAverageColor keeps the stored linear reflectivity as the shader's pivot.
func TestAverageColor(t *testing.T) {
	// Encode distinct channels before the dimensions in a compiled texture header.
	header := make([]byte, 40)
	average := [3]float32{0.125, 0.375, 0.75}
	for i, value := range average {
		binary.LittleEndian.PutUint32(header[4+i*4:], math.Float32bits(value))
	}
	binary.LittleEndian.PutUint16(header[20:], 4)
	binary.LittleEndian.PutUint16(header[22:], 4)
	header[26], header[27] = byte(FormatRGBA8888), 1
	compiled, err := resource.Write(1, []resource.Block{{Tag: "DATA", Data: header}})
	if err != nil {
		t.Fatal(err)
	}

	// Read the RGB values without gamma correction or overlapping the dimensions.
	read, err := Read(compiled)
	if err != nil {
		t.Fatal(err)
	}
	if read.AverageColor != average || read.Width != 4 || read.Height != 4 {
		t.Fatalf("texture header = %+v; want average %v and 4x4", read, average)
	}
}
