package tools

import (
	"encoding/binary"
	"encoding/json"
	"net/http"
	"path"
	"strconv"
	"strings"

	"github.com/klauspost/compress/zstd"
	"github.com/paralin/modlock/source2/kv3"
	"github.com/paralin/modlock/source2/resource"
	"github.com/paralin/modlock/source2/vpk"
	"github.com/pkg/errors"
)

// Assets serves the game's own files from the player's install, read where
// they lie: it never copies, converts or caches them on disk. The page
// decodes vertex and index buffers and uploads textures in their stored GPU
// formats, so the server does no pixel or vertex work.
type Assets struct {
	// archive is the game's pak01_dir.vpk.
	archive *vpk.Archive
}

// OpenAssets opens the archive at pak, such as
// game/citadel/pak01_dir.vpk in the Deadlock install.
func OpenAssets(pak string) (*Assets, error) {
	archive, err := vpk.OpenArchive(pak)
	if err != nil {
		return nil, err
	}
	return &Assets{archive: archive}, nil
}

// routes adds the asset routes to mux.
func (a *Assets) routes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/assets/list", a.list)
	mux.HandleFunc("GET /api/assets/model/{name...}", a.model)
	mux.HandleFunc("GET /api/assets/buffer/{mesh}/{kind}/{index}/{name...}", a.buffer)
	mux.HandleFunc("GET /api/assets/texture/{name...}", a.texture)
	mux.HandleFunc("GET /api/assets/mip/{mip}/{name...}", a.mip)
	mux.HandleFunc("GET /api/assets/kv3/{name...}", a.kv3)
}

// list answers every file name in the archive with its size.
func (a *Assets) list(w http.ResponseWriter, r *http.Request) {
	names := a.archive.Names()
	sizes := make([]int64, len(names))
	for i, name := range names {
		sizes[i], _ = a.archive.Size(name)
	}
	writeJSON(w, map[string]any{"names": names, "sizes": sizes})
}

// read reads a compiled resource's blocks.
func (a *Assets) read(name string) ([]byte, []resource.Block, error) {
	data, err := a.archive.ReadFile(name)
	if err != nil {
		return nil, nil, err
	}
	blocks, err := resource.Read(data)
	return data, blocks, err
}

// decodeBlock decodes the KeyValues3 block at index.
func decodeBlock(blocks []resource.Block, index int) (map[string]any, error) {
	if index < 0 || index >= len(blocks) {
		return nil, errors.Errorf("no block %d", index)
	}
	value, err := kv3.Decode(blocks[index].Data)
	if err != nil {
		return nil, err
	}
	object, _ := value.(map[string]any)
	return object, nil
}

// decodeTag decodes the first KeyValues3 block tagged tag.
func decodeTag(blocks []resource.Block, tag string) (map[string]any, error) {
	for index, block := range blocks {
		if block.Tag == tag {
			return decodeBlock(blocks, index)
		}
	}
	return nil, errors.Errorf("no %s block", tag)
}

// meshBuffer is one vertex or index buffer of a model's mesh, in either of
// the two layouts compiled models use: its own block per buffer, described in
// CTRL, or every buffer of the mesh packed in one MBUF block.
type meshBuffer struct {
	Count   int          `json:"count"`
	Stride  int          `json:"stride"`
	Meshopt bool         `json:"meshopt"`
	Fields  []inputField `json:"fields"`
	// data is the buffer's stored bytes, zstd still applied when zstd is set.
	data []byte
	zstd bool
}

// inputField is one vertex attribute: its semantic, DXGI format and offset.
type inputField struct {
	Semantic string `json:"semantic"`
	Index    int    `json:"index"`
	Format   int    `json:"format"`
	Offset   int    `json:"offset"`
}

// mesh is one drawable mesh of a model: its buffers and its MDAT draw data.
type mesh struct {
	name          string
	vertex, index []meshBuffer
	data          map[string]any
}

// meshes reads the meshes a model draws by default, at its finest level of
// detail and in its default mesh groups. A mesh it cannot read is left out.
func meshes(blocks []resource.Block) ([]mesh, error) {
	ctrl, err := decodeTag(blocks, "CTRL")
	if err != nil {
		return nil, err
	}
	// DATA holds each mesh's level of detail and mesh group masks, by mesh
	// index; a model without them draws every mesh.
	data, _ := decodeTag(blocks, "DATA")
	lods, groups := list(data["m_refLODGroupMasks"]), list(data["m_refMeshGroupMasks"])
	defaults := uint64(number(data["m_nDefaultMeshGroupMask"]))
	drawn := func(index int) bool {
		if index < len(lods) && uint64(number(lods[index]))&1 == 0 {
			return false
		}
		return index >= len(groups) || defaults == 0 || uint64(number(groups[index]))&defaults != 0
	}
	var out []mesh
	for _, item := range list(ctrl["embedded_meshes"]) {
		desc, _ := item.(map[string]any)
		if desc == nil {
			continue
		}
		m := mesh{}
		var dataBlock, meshIndex any
		if desc["vbib_block"] != nil {
			// MBUF layout: snake_case names and one block of buffers.
			name, _ := desc["name"].(string)
			m.name, dataBlock, meshIndex = name, desc["data_block"], desc["mesh_index"]
			index := int(number(desc["vbib_block"]))
			if index < 0 || index >= len(blocks) {
				continue
			}
			if m.vertex, m.index, err = readVBIB(blocks[index].Data); err != nil {
				continue
			}
		} else {
			name, _ := desc["m_Name"].(string)
			m.name, dataBlock, meshIndex = name, desc["m_nDataBlock"], desc["m_nMeshIndex"]
			m.vertex = kv3Buffers(blocks, desc["m_vertexBuffers"])
			m.index = kv3Buffers(blocks, desc["m_indexBuffers"])
		}
		if dataBlock == nil || !drawn(int(number(meshIndex))) {
			continue
		}
		if m.data, err = decodeBlock(blocks, int(number(dataBlock))); err != nil {
			continue
		}
		out = append(out, m)
	}
	return out, nil
}

// kv3Buffers reads buffers that CTRL describes, each in its own block.
func kv3Buffers(blocks []resource.Block, value any) []meshBuffer {
	var out []meshBuffer
	for _, item := range list(value) {
		desc, _ := item.(map[string]any)
		index := int(number(desc["m_nBlockIndex"]))
		buffer := meshBuffer{
			Count:   int(number(desc["m_nElementCount"])),
			Stride:  int(number(desc["m_nElementSizeInBytes"])),
			Meshopt: desc["m_bMeshoptCompressed"] == true,
			zstd:    desc["m_bCompressedZSTD"] == true,
		}
		if index >= 0 && index < len(blocks) {
			buffer.data = blocks[index].Data
		}
		for _, f := range list(desc["m_inputLayoutFields"]) {
			field, _ := f.(map[string]any)
			semantic, _ := field["m_pSemanticName"].(string)
			buffer.Fields = append(buffer.Fields, inputField{
				Semantic: strings.ToUpper(semantic),
				Index:    int(number(field["m_nSemanticIndex"])),
				Format:   int(number(field["m_Format"])),
				Offset:   int(number(field["m_nOffset"])),
			})
		}
		out = append(out, buffer)
	}
	return out
}

// readVBIB reads an MBUF block: a header of two (offset, count) tables, one
// of vertex and one of index buffers, each entry 24 bytes with offsets
// relative to the field that holds them.
func readVBIB(b []byte) (vertex, index []meshBuffer, err error) {
	u32 := func(at int) (int, error) {
		if at < 0 || at+4 > len(b) {
			return 0, errors.New("the buffer block is truncated")
		}
		return int(binary.LittleEndian.Uint32(b[at:])), nil
	}
	table := func(at int) ([]meshBuffer, error) {
		offset, err := u32(at)
		if err != nil {
			return nil, err
		}
		count, err := u32(at + 4)
		if err != nil {
			return nil, err
		}
		out := make([]meshBuffer, 0, count)
		for i, entry := 0, at+offset; i < count; i, entry = i+1, entry+24 {
			var v [6]int
			for j := range v {
				if v[j], err = u32(entry + 4*j); err != nil {
					return nil, err
				}
			}
			size := v[1]
			buffer := meshBuffer{
				Count:   v[0],
				Stride:  size & 0x3ffffff,
				Meshopt: size&0x4000000 == 0,
				zstd:    size&0x8000000 != 0,
			}
			fields := entry + 8 + v[2]
			for f := 0; f < v[3]; f++ {
				at := fields + 56*f
				if at+56 > len(b) {
					return nil, errors.New("the buffer block is truncated")
				}
				name, _, _ := strings.Cut(string(b[at:at+32]), "\x00")
				buffer.Fields = append(buffer.Fields, inputField{
					Semantic: strings.ToUpper(name),
					Index:    int(binary.LittleEndian.Uint32(b[at+32:])),
					Format:   int(binary.LittleEndian.Uint32(b[at+36:])),
					Offset:   int(binary.LittleEndian.Uint32(b[at+40:])),
				})
			}
			start := entry + 16 + v[4]
			if start < 0 || start+v[5] > len(b) {
				return nil, errors.New("the buffer block is truncated")
			}
			buffer.data = b[start : start+v[5]]
			// Stored at full size, a buffer is neither meshopt nor zstd encoded.
			if len(buffer.data) >= buffer.Count*buffer.Stride {
				buffer.Meshopt, buffer.zstd = false, false
			}
			out = append(out, buffer)
		}
		return out, nil
	}
	if vertex, err = table(0); err != nil {
		return nil, nil, err
	}
	index, err = table(8)
	return vertex, index, err
}

// model answers the meshes a model draws by default, each with its buffer
// layouts and its MDAT scene objects, and the texture parameters of every
// material the draw calls name. The page fetches each buffer by position.
func (a *Assets) model(w http.ResponseWriter, r *http.Request) {
	_, blocks, err := a.read(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	found, err := meshes(blocks)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnprocessableEntity)
		return
	}
	out := make([]any, 0, len(found))
	materials := map[string]any{}
	for _, m := range found {
		out = append(out, map[string]any{
			"name":          m.name,
			"vertexBuffers": m.vertex,
			"indexBuffers":  m.index,
			"sceneObjects":  m.data["m_sceneObjects"],
		})
		for _, object := range list(m.data["m_sceneObjects"]) {
			for _, call := range list(object.(map[string]any)["m_drawCalls"]) {
				material, _ := call.(map[string]any)["m_material"].(string)
				if _, seen := materials[material]; material != "" && !seen {
					materials[material] = a.material(material)
				}
			}
		}
	}
	writeJSON(w, map[string]any{"meshes": out, "materials": materials})
}

// material returns a material's shader and texture parameters, or nil when
// it cannot be read.
func (a *Assets) material(name string) any {
	_, blocks, err := a.read(name + "_c")
	if err != nil {
		return nil
	}
	data, err := decodeTag(blocks, "DATA")
	if err != nil {
		return nil
	}
	textures := map[string]string{}
	for _, item := range list(data["m_textureParams"]) {
		param, _ := item.(map[string]any)
		if value, _ := param["m_pValue"].(string); value != "" {
			textures[param["m_name"].(string)] = value + "_c"
		}
	}
	return map[string]any{"shader": data["m_shaderName"], "textures": textures}
}

// buffer answers one mesh buffer's bytes, meshopt-encoded when the model
// says so; the page decodes them. The mesh is its position in the model
// response and kind is "vertex" or "index".
func (a *Assets) buffer(w http.ResponseWriter, r *http.Request) {
	_, blocks, err := a.read(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	found, _ := meshes(blocks)
	m, _ := strconv.Atoi(r.PathValue("mesh"))
	i, _ := strconv.Atoi(r.PathValue("index"))
	if m < 0 || m >= len(found) {
		http.Error(w, "no such mesh", http.StatusNotFound)
		return
	}
	buffers := found[m].vertex
	if r.PathValue("kind") == "index" {
		buffers = found[m].index
	}
	if i < 0 || i >= len(buffers) {
		http.Error(w, "no such buffer", http.StatusNotFound)
		return
	}
	data := buffers[i].data
	if buffers[i].zstd {
		if data, err = unzstd(data); err != nil {
			http.Error(w, err.Error(), http.StatusUnprocessableEntity)
			return
		}
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "max-age=3600")
	_, _ = w.Write(data)
}

// unzstd removes a buffer's zstd layer.
func unzstd(data []byte) ([]byte, error) {
	decoder, err := zstd.NewReader(nil)
	if err != nil {
		return nil, err
	}
	defer decoder.Close()
	return decoder.DecodeAll(data, nil)
}

// textureHeader is the fixed part of a texture's DATA block.
type textureHeader struct {
	Width, Height, Depth int
	Format, Mips, Flags  int
	// pixels is where the mips start in the file, smallest mip first.
	pixels int
}

// readTexture reads a texture's header and the file holding its mips.
func (a *Assets) readTexture(name string) ([]byte, textureHeader, error) {
	data, blocks, err := a.read(name)
	if err != nil {
		return nil, textureHeader{}, err
	}
	d, ok := resource.Find(blocks, "DATA")
	if !ok || len(d) < 40 {
		return nil, textureHeader{}, errors.New("no texture header")
	}
	header := textureHeader{
		Flags:  int(binary.LittleEndian.Uint16(d[2:])),
		Width:  int(binary.LittleEndian.Uint16(d[20:])),
		Height: int(binary.LittleEndian.Uint16(d[22:])),
		Depth:  max(1, int(binary.LittleEndian.Uint16(d[24:]))),
		Format: int(d[26]),
		Mips:   max(1, int(d[27])),
		// The DATA block aliases data, so its end is where the mips start.
		pixels: len(data) - cap(d) + len(d),
	}
	return data, header, nil
}

// texture answers a texture's header.
func (a *Assets) texture(w http.ResponseWriter, r *http.Request) {
	_, header, err := a.readTexture(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	writeJSON(w, map[string]any{
		"width": header.Width, "height": header.Height, "depth": header.Depth,
		"format": header.Format, "mips": header.Mips, "flags": header.Flags,
	})
}

// mipSize returns the bytes one mip of the texture takes, and false for a
// format the page cannot upload as stored.
func mipSize(header textureHeader, mip int) (int, bool) {
	w, h := max(1, header.Width>>mip), max(1, header.Height>>mip)
	d := max(1, header.Depth>>mip)
	if header.Flags&4 != 0 {
		d *= 6
	}
	blocks := func(bytes int) int { return (w + 3) / 4 * ((h + 3) / 4) * bytes * d }
	switch header.Format {
	case 1, 27: // DXT1, ATI1N (BC4)
		return blocks(8), true
	case 2, 19, 20, 21: // DXT5, BC6H, BC7, ATI2N (BC5)
		return blocks(16), true
	case 4, 28: // RGBA8888, BGRA8888
		return w * h * d * 4, true
	default:
		return 0, false
	}
}

// mip answers one mip's stored bytes, a range of the file read in place.
func (a *Assets) mip(w http.ResponseWriter, r *http.Request) {
	mip, _ := strconv.Atoi(r.PathValue("mip"))
	data, header, err := a.readTexture(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	if mip < 0 || mip >= header.Mips {
		http.Error(w, "no such mip", http.StatusNotFound)
		return
	}
	at := header.pixels
	for smaller := header.Mips - 1; smaller > mip; smaller-- {
		size, ok := mipSize(header, smaller)
		if !ok {
			http.Error(w, "format "+strconv.Itoa(header.Format)+" is not served yet", http.StatusUnprocessableEntity)
			return
		}
		at += size
	}
	size, _ := mipSize(header, mip)
	if at+size > len(data) {
		http.Error(w, "the texture is shorter than its mips", http.StatusUnprocessableEntity)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "max-age=3600")
	_, _ = w.Write(data[at : at+size])
}

// kv3 answers a resource's KeyValues3 blocks as JSON, by tag.
func (a *Assets) kv3(w http.ResponseWriter, r *http.Request) {
	_, blocks, err := a.read(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	out := map[string]any{}
	tags := make([]string, 0, len(blocks))
	for _, block := range blocks {
		tags = append(tags, block.Tag)
		if value, err := kv3.Decode(block.Data); err == nil {
			out[block.Tag] = value
		}
	}
	writeJSON(w, map[string]any{"blocks": tags, "kv3": out, "ext": strings.TrimPrefix(path.Ext(r.PathValue("name")), ".")})
}

// list returns value as a list, or nil.
func list(value any) []any {
	items, _ := value.([]any)
	return items
}

// number returns a decoded KeyValues3 number as a float.
func number(value any) float64 {
	switch v := value.(type) {
	case int64:
		return float64(v)
	case uint64:
		return float64(v)
	case float64:
		return v
	}
	return 0
}

// writeJSON writes value as the response.
func writeJSON(w http.ResponseWriter, value any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(value)
}
