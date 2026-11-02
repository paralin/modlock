package studio

import (
	"bytes"
	"encoding/binary"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"

	"github.com/paralin/modlock/source2/resource"
)

// TestSoundPlayback checks compiled headers, intact audio and truncated payloads.
func TestSoundPlayback(t *testing.T) {
	for _, encoding := range []byte{0, 1, 2, 3} {
		header := make([]byte, 48)
		binary.LittleEndian.PutUint16(header, 44100)
		header[2], header[3] = encoding, 2
		binary.LittleEndian.PutUint32(header[12:], math.Float32bits(1.25))
		binary.LittleEndian.PutUint32(header[28:], 4)
		data, err := resource.Write(4, []resource.Block{{Tag: "DATA", Data: header}})
		if err != nil {
			t.Fatal(err)
		}
		data = append(data, 1, 2, 3, 4)
		desc, audio, err := readSound(data)
		if err != nil {
			t.Fatal(err)
		}
		if desc.SampleRate != 44100 || desc.Channels != 2 || desc.Duration != 1.25 {
			t.Fatalf("metadata: %+v", desc)
		}
		switch encoding {
		case 0, 1:
			if string(audio[:4]) != "RIFF" || binary.LittleEndian.Uint32(audio[40:]) != 4 || string(audio[44:]) != "\x01\x02\x03\x04" {
				t.Fatalf("bad WAVE: %x", audio)
			}
		case 2:
			if string(audio) != "\x01\x02\x03\x04" {
				t.Fatalf("changed MP3: %x", audio)
			}
		case 3:
			if desc.Unavailable == "" || len(audio) != 0 {
				t.Fatal("ADPCM offered for playback")
			}
		}
		if encoding != 3 {
			if _, _, err := readSound(data[:len(data)-1]); err == nil {
				t.Fatal("accepted truncated audio")
			}
		}
	}
}

// TestSoundArchive exercises real archive metadata, payloads and HTTP seeking.
func TestSoundArchive(t *testing.T) {
	game := os.Getenv("STUDIO_TEST_GAME")
	if game == "" {
		t.Skip("set STUDIO_TEST_GAME to game/citadel")
	}
	assets := OpenAssets(game)
	if assets.Err() != nil {
		t.Fatal(assets.Err())
	}
	handler := Handler(t.TempDir(), assets)
	count := 0
	for _, name := range assets.archives[0].Names() {
		if !strings.HasSuffix(name, ".vsnd_c") {
			continue
		}
		data, err := assets.archives[0].ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		desc, audio, err := readSound(data)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if desc.Unavailable != "" {
			continue
		}
		request := httptest.NewRequest("GET", "/api/assets/sound/"+name+"?audio=1", nil)
		request.Header.Set("Range", "bytes=0-3")
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != http.StatusPartialContent || !bytes.Equal(response.Body.Bytes(), audio[:4]) {
			t.Fatalf("range: %d %q", response.Code, response.Body.String())
		}
		t.Logf("%s: v%d %s %.2fs %dHz %dch", name, binary.LittleEndian.Uint16(data[6:]), desc.Format, desc.Duration, desc.SampleRate, desc.Channels)
		count++
		if count == 3 {
			return
		}
	}
	t.Fatal("archive did not contain three playable sounds")
}
