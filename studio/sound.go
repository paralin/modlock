package studio

import (
	"bytes"
	"encoding/binary"
	"math"
	"net/http"
	"time"

	"github.com/paralin/modlock/source2/resource"
	"github.com/pkg/errors"
)

// readSound reads the sound header and its appended audio, without transcoding.
// The container's declared size excludes streaming audio, unlike its blocks.
func readSound(data []byte) (*Sound, []byte, error) {
	// Read the resource before locating its trailing audio.
	blocks, err := resource.Read(data)
	if err != nil {
		return nil, nil, err
	}
	out := &Sound{}
	var size uint32
	version := binary.LittleEndian.Uint16(data[6:])

	// Version 5 stores voice-container metadata in CTRL instead of DATA.
	if version >= 5 {
		ctrl, err := decodeTag(blocks, "CTRL")
		if err != nil {
			return nil, nil, err
		}
		sound, ok := ctrl["m_vSound"].(map[string]any)
		if !ok {
			out.Unavailable = "This sound combines other sounds. Choose a sound file to preview."
			return out, nil, nil
		}
		out.Format, _ = sound["m_nFormat"].(string)
		out.SampleRate = uint32(number(sound["m_nRate"]))
		out.Channels = uint32(number(sound["m_nChannels"]))
		out.Duration = float32(number(sound["m_flDuration"]))
		size = uint32(number(sound["m_nStreamingSize"]))
	} else {
		// Older resources keep a packed header followed by loop and stream fields.
		header, ok := resource.Find(blocks, "DATA")
		if !ok || len(header) < 32 {
			return nil, nil, errors.New("The sound header is truncated")
		}
		if version == 4 {
			out.SampleRate = uint32(binary.LittleEndian.Uint16(header))
			out.Channels = uint32(header[3])
			switch header[2] {
			case 0:
				out.Format = "PCM16"
			case 1:
				out.Format = "PCM8"
			case 2:
				out.Format = "MP3"
			case 3:
				out.Format = "ADPCM"
			}
		} else {
			info := binary.LittleEndian.Uint32(header)
			out.SampleRate = info >> 14 & 0xffff
			out.Channels = info >> 7 & 3
			switch {
			case info&3 == 2:
				out.Format = "MP3"
			case info>>12&3 == 2:
				out.Format = "ADPCM"
			case info>>2&31 == 8:
				out.Format = "PCM8"
			default:
				out.Format = "PCM16"
			}
		}
		out.Duration = math.Float32frombits(binary.LittleEndian.Uint32(header[12:]))
		size = binary.LittleEndian.Uint32(header[28:])
	}

	// Unsupported encodings remain inspectable without offering dead controls.
	if out.Format != "MP3" && out.Format != "PCM8" && out.Format != "PCM16" {
		out.Unavailable = "This sound's encoding cannot be previewed. Choose another sound."
		return out, nil, nil
	}
	start := uint64(binary.LittleEndian.Uint32(data))
	if size == 0 || start > uint64(len(data)) || uint64(size) > uint64(len(data))-start {
		return nil, nil, errors.New("The sound's audio is missing or truncated")
	}
	audio := data[int(start) : int(start)+int(size)]
	if out.Format == "MP3" {
		return out, audio, nil
	}
	if out.SampleRate == 0 || out.Channels == 0 || out.Channels > 8 {
		return nil, nil, errors.New("The sound has an invalid sample rate or channel count")
	}

	// Add a standard PCM WAVE header; leave the samples in their stored encoding.
	bits := uint16(16)
	if out.Format == "PCM8" {
		bits = 8
	}
	align := uint16(out.Channels) * bits / 8

	// Write the RIFF container and fixed-size PCM format chunk.
	wave := make([]byte, 0, 44+len(audio)+len(audio)%2)
	wave = append(wave, "RIFF"...)
	wave = binary.LittleEndian.AppendUint32(wave, 36+size+size%2)
	wave = append(wave, "WAVEfmt "...)
	wave = binary.LittleEndian.AppendUint32(wave, 16)
	wave = binary.LittleEndian.AppendUint16(wave, 1)
	wave = binary.LittleEndian.AppendUint16(wave, uint16(out.Channels))

	// Describe each sample frame in the original channel layout.
	wave = binary.LittleEndian.AppendUint32(wave, out.SampleRate)
	wave = binary.LittleEndian.AppendUint32(wave, out.SampleRate*uint32(align))
	wave = binary.LittleEndian.AppendUint16(wave, align)
	wave = binary.LittleEndian.AppendUint16(wave, bits)

	// Append the stored samples and pad an odd-sized chunk.
	wave = append(wave, "data"...)
	wave = binary.LittleEndian.AppendUint32(wave, size)
	wave = append(wave, audio...)
	if size%2 != 0 {
		wave = append(wave, 0)
	}
	return out, wave, nil
}

// sound answers metadata or browser-playable audio for one local archive sound.
func (a *Assets) sound(w http.ResponseWriter, r *http.Request) {
	// Read only the selected archive entry; sounds are never cached on disk.
	name := r.PathValue("name")
	data, err := a.archiveOf(name).ReadFile(name)
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	sound, audio, err := readSound(data)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnprocessableEntity)
		return
	}
	if r.URL.Query().Get("audio") != "1" {
		writeMessage(w, sound)
		return
	}
	if sound.Unavailable != "" {
		http.Error(w, sound.Unavailable, http.StatusUnprocessableEntity)
		return
	}

	// ServeContent handles byte ranges used by the browser to seek and resume.
	mime, extension := "audio/wav", ".wav"
	if sound.Format == "MP3" {
		mime, extension = "audio/mpeg", ".mp3"
	}
	w.Header().Set("Content-Type", mime)
	http.ServeContent(w, r, "sound"+extension, time.Time{}, bytes.NewReader(audio))
}
