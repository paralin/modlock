package studio

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"encoding/json"
	"math/bits"
	"net/http"
	"path"
	"slices"
	"strconv"
	"strings"
	"sync"

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
	// archive is the game's pak01_dir.vpk, or nil when err says why it could
	// not be opened.
	archive *vpk.Archive
	err     error

	// listing is the gzipped Listing, built on the first request.
	listing func() ([]byte, error)

	// mu guards last, the meshes of the model read last, which its buffer
	// requests read again one after another.
	mu   sync.Mutex
	last struct {
		name   string
		meshes []mesh
	}
}

// OpenAssets opens the archive at pak, such as
// game/citadel/pak01_dir.vpk in the Deadlock install. When it cannot, every
// asset route answers why, so the page can say it.
func OpenAssets(pak string) *Assets {
	archive, err := vpk.OpenArchive(pak)
	a := &Assets{archive: archive, err: err}
	a.listing = sync.OnceValues(a.buildListing)
	return a
}

// Unavailable returns Assets whose routes all answer err, for a player whose
// install could not be found.
func Unavailable(err error) *Assets {
	return &Assets{err: err}
}

// Err returns why the archive could not be opened, or nil.
func (a *Assets) Err() error {
	return a.err
}

// routes adds the asset routes to mux.
func (a *Assets) routes(mux *http.ServeMux) {
	if a.err != nil {
		mux.HandleFunc("GET /api/assets/", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, a.err.Error(), http.StatusServiceUnavailable)
		})
		return
	}
	mux.HandleFunc("GET /api/assets/list", a.list)
	mux.HandleFunc("GET /api/assets/model/{name...}", a.model)
	mux.HandleFunc("GET /api/assets/buffer/{mesh}/{kind}/{index}/{name...}", a.buffer)
	mux.HandleFunc("GET /api/assets/texture/{name...}", a.texture)
	mux.HandleFunc("GET /api/assets/mip/{mip}/{name...}", a.mip)
	mux.HandleFunc("GET /api/assets/resource/{name...}", a.resource)
}

// list answers the archive's Listing, gzipped.
func (a *Assets) list(w http.ResponseWriter, r *http.Request) {
	data, err := a.listing()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Encoding", "gzip")
	writeBytes(w, "application/x-protobuf", data)
}

// buildListing encodes and gzips the archive's file list, grouped by
// directory.
func (a *Assets) buildListing() ([]byte, error) {
	// Sort the names so each directory's files are adjacent.
	names := a.archive.Names()
	slices.Sort(names)
	listing := &Listing{Files: make([]*File, 0, len(names))}
	dirs := map[string]uint32{}
	for _, name := range names {
		dir, base := path.Split(name)
		dir = strings.TrimSuffix(dir, "/")
		index, ok := dirs[dir]
		if !ok {
			index = uint32(len(listing.Dirs))
			dirs[dir] = index
			listing.Dirs = append(listing.Dirs, dir)
		}
		size, _ := a.archive.Size(name)
		listing.Files = append(listing.Files, &File{Dir: index, Name: base, Size: uint64(size)})
	}

	// Encode and compress it once for every page that asks.
	data, err := listing.MarshalVT()
	if err != nil {
		return nil, err
	}
	var out bytes.Buffer
	zw, _ := gzip.NewWriterLevel(&out, gzip.BestCompression)
	_, _ = zw.Write(data)
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
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

// mesh is one mesh of a model as the page sees it, with each buffer's
// stored bytes beside its description.
type mesh struct {
	desc          *Mesh
	vertex, index []storedBuffer
}

// storedBuffer is a buffer's stored bytes, zstd still applied when zstd is
// set.
type storedBuffer struct {
	data []byte
	zstd bool
}

// meshes reads every mesh of a model, in every level of detail and mesh
// group, with the masks that select them. A mesh it cannot read is left out.
func meshes(blocks []resource.Block) ([]mesh, error) {
	ctrl, err := decodeTag(blocks, "CTRL")
	if err != nil {
		return nil, err
	}
	// DATA holds each mesh's level of detail and mesh group masks, by mesh
	// index. A mesh without them draws at the finest level in every group.
	data, _ := decodeTag(blocks, "DATA")
	lods, groups := list(data["m_refLODGroupMasks"]), list(data["m_refMeshGroupMasks"])
	var out []mesh
	for _, item := range list(ctrl["embedded_meshes"]) {
		desc, _ := item.(map[string]any)
		if desc == nil {
			continue
		}
		m, dataBlock, meshIndex, ok := readMesh(blocks, desc)
		if !ok {
			continue
		}
		m.desc.LodMask, m.desc.GroupMask = 1, ^uint64(0)
		if meshIndex < len(lods) {
			m.desc.LodMask = uint64(number(lods[meshIndex]))
		}
		if meshIndex < len(groups) {
			m.desc.GroupMask = uint64(number(groups[meshIndex]))
		}
		mdat, err := decodeBlock(blocks, dataBlock)
		if err != nil {
			continue
		}
		m.desc.DrawCalls = drawCalls(mdat)
		out = append(out, m)
	}
	return out, nil
}

// readMesh reads one embedded mesh's buffers in either of the two layouts
// compiled models use: its own block per buffer, described in CTRL, or every
// buffer of the mesh packed in one MBUF block. It returns the mesh, the
// block of its draw data and its index in DATA's masks.
func readMesh(blocks []resource.Block, desc map[string]any) (mesh, int, int, bool) {
	// MBUF layout: snake_case names and one block of buffers.
	if desc["vbib_block"] != nil {
		name, _ := desc["name"].(string)
		m := mesh{desc: &Mesh{Name: name}}
		index := int(number(desc["vbib_block"]))
		if index < 0 || index >= len(blocks) || desc["data_block"] == nil {
			return mesh{}, 0, 0, false
		}
		if err := readVBIB(blocks[index].Data, &m); err != nil {
			return mesh{}, 0, 0, false
		}
		return m, int(number(desc["data_block"])), int(number(desc["mesh_index"])), true
	}

	// KV3 layout: each buffer in its own block.
	if desc["m_nDataBlock"] == nil {
		return mesh{}, 0, 0, false
	}
	name, _ := desc["m_Name"].(string)
	m := mesh{desc: &Mesh{Name: name}}
	m.desc.VertexBuffers, m.vertex = kv3Buffers(blocks, desc["m_vertexBuffers"])
	m.desc.IndexBuffers, m.index = kv3Buffers(blocks, desc["m_indexBuffers"])
	return m, int(number(desc["m_nDataBlock"])), int(number(desc["m_nMeshIndex"])), true
}

// drawCalls reads every draw call of a mesh's scene objects from its MDAT
// block.
func drawCalls(mdat map[string]any) []*DrawCall {
	var out []*DrawCall
	for _, object := range list(mdat["m_sceneObjects"]) {
		calls, _ := object.(map[string]any)
		for _, item := range list(calls["m_drawCalls"]) {
			call, _ := item.(map[string]any)
			material, _ := call["m_material"].(string)
			var vertex any
			if buffers := list(call["m_vertexBuffers"]); len(buffers) != 0 {
				first, _ := buffers[0].(map[string]any)
				vertex = first["m_hBuffer"]
			}
			index, _ := call["m_indexBuffer"].(map[string]any)
			out = append(out, &DrawCall{
				Material:     material,
				VertexBuffer: uint32(number(vertex)),
				IndexBuffer:  uint32(number(index["m_hBuffer"])),
				StartIndex:   uint32(number(call["m_nStartIndex"])),
				IndexCount:   uint32(number(call["m_nIndexCount"])),
				BaseVertex:   int32(number(call["m_nBaseVertex"])),
			})
		}
	}
	return out
}

// kv3Buffers reads buffers that CTRL describes, each in its own block.
func kv3Buffers(blocks []resource.Block, value any) ([]*Buffer, []storedBuffer) {
	var descs []*Buffer
	var stored []storedBuffer
	for _, item := range list(value) {
		// Describe the buffer and find its block.
		desc, _ := item.(map[string]any)
		buffer := &Buffer{
			Count:   uint32(number(desc["m_nElementCount"])),
			Stride:  uint32(number(desc["m_nElementSizeInBytes"])),
			Meshopt: desc["m_bMeshoptCompressed"] == true,
		}
		s := storedBuffer{zstd: desc["m_bCompressedZSTD"] == true}
		if index := int(number(desc["m_nBlockIndex"])); index >= 0 && index < len(blocks) {
			s.data = blocks[index].Data
		}

		// Read its vertex attributes.
		for _, f := range list(desc["m_inputLayoutFields"]) {
			field, _ := f.(map[string]any)
			semantic, _ := field["m_pSemanticName"].(string)
			buffer.Fields = append(buffer.Fields, &Field{
				Semantic: strings.ToUpper(semantic),
				Index:    uint32(number(field["m_nSemanticIndex"])),
				Format:   uint32(number(field["m_Format"])),
				Offset:   uint32(number(field["m_nOffset"])),
			})
		}
		descs, stored = append(descs, buffer), append(stored, s)
	}
	return descs, stored
}

// readVBIB reads an MBUF block into m: a header of two (offset, count)
// tables, one of vertex and one of index buffers, each entry 24 bytes with
// offsets relative to the field that holds them.
func readVBIB(b []byte, m *mesh) error {
	u32 := func(at int) (int, error) {
		if at < 0 || at+4 > len(b) {
			return 0, errors.New("the buffer block is truncated")
		}
		return int(binary.LittleEndian.Uint32(b[at:])), nil
	}
	table := func(at int) ([]*Buffer, []storedBuffer, error) {
		// Read the table's place and length.
		offset, err := u32(at)
		if err != nil {
			return nil, nil, err
		}
		count, err := u32(at + 4)
		if err != nil {
			return nil, nil, err
		}

		// Read each entry: count, size and flags, fields and data.
		var descs []*Buffer
		var stored []storedBuffer
		for i, entry := 0, at+offset; i < count; i, entry = i+1, entry+24 {
			var v [6]int
			for j := range v {
				if v[j], err = u32(entry + 4*j); err != nil {
					return nil, nil, err
				}
			}
			buffer, s, err := vbibEntry(b, entry, v)
			if err != nil {
				return nil, nil, err
			}
			descs, stored = append(descs, buffer), append(stored, s)
		}
		return descs, stored, nil
	}
	var err error
	if m.desc.VertexBuffers, m.vertex, err = table(0); err != nil {
		return err
	}
	m.desc.IndexBuffers, m.index, err = table(8)
	return err
}

// vbibEntry reads one MBUF buffer entry at entry, whose six words are v:
// count, size with flags, field offset, field count, data offset and data
// size.
func vbibEntry(b []byte, entry int, v [6]int) (*Buffer, storedBuffer, error) {
	// The size word carries the stride and the compression flags.
	size := v[1]
	buffer := &Buffer{
		Count:   uint32(v[0]),
		Stride:  uint32(size & 0x3ffffff),
		Meshopt: size&0x4000000 == 0,
	}
	s := storedBuffer{zstd: size&0x8000000 != 0}

	// Each field is 56 bytes: a 32 byte name, then index, format and offset.
	fields := entry + 8 + v[2]
	for f := 0; f < v[3]; f++ {
		at := fields + 56*f
		if at < 0 || at+56 > len(b) {
			return nil, s, errors.New("the buffer block is truncated")
		}
		name, _, _ := strings.Cut(string(b[at:at+32]), "\x00")
		buffer.Fields = append(buffer.Fields, &Field{
			Semantic: strings.ToUpper(name),
			Index:    binary.LittleEndian.Uint32(b[at+32:]),
			Format:   binary.LittleEndian.Uint32(b[at+36:]),
			Offset:   binary.LittleEndian.Uint32(b[at+40:]),
		})
	}

	// Stored at full size, a buffer is neither meshopt nor zstd encoded.
	start := entry + 16 + v[4]
	if start < 0 || start+v[5] > len(b) {
		return nil, s, errors.New("the buffer block is truncated")
	}
	s.data = b[start : start+v[5]]
	if len(s.data) >= int(buffer.Count)*int(buffer.Stride) {
		buffer.Meshopt, s.zstd = false, false
	}
	return buffer, s, nil
}

// modelMeshes returns a model's meshes, reading the model only when it is
// not the one read last.
func (a *Assets) modelMeshes(name string) ([]mesh, error) {
	// Answer a buffer request from the model read last.
	a.mu.Lock()
	if a.last.name == name {
		found := a.last.meshes
		a.mu.Unlock()
		return found, nil
	}
	a.mu.Unlock()

	// Read the model and keep it for its other buffer requests.
	_, blocks, err := a.read(name)
	if err != nil {
		return nil, err
	}
	found, err := meshes(blocks)
	if err != nil {
		return nil, err
	}
	a.remember(name, found)
	return found, nil
}

// remember keeps a model's meshes as the model read last.
func (a *Assets) remember(name string, found []mesh) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.last.name, a.last.meshes = name, found
}

// model answers a model's meshes, mesh groups, levels of detail and the
// texture parameters of every material its draw calls name. The page
// fetches each buffer by position.
func (a *Assets) model(w http.ResponseWriter, r *http.Request) {
	// Read the model's meshes and keep them for its buffer requests.
	name := r.PathValue("name")
	_, blocks, err := a.read(name)
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	found, err := meshes(blocks)
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnprocessableEntity)
		return
	}
	a.remember(name, found)

	// Describe the meshes and count the levels of detail.
	data, _ := decodeTag(blocks, "DATA")
	out := &Model{
		DefaultGroups: uint64(number(data["m_nDefaultMeshGroupMask"])),
		Lods:          1,
		Materials:     map[string]*Material{},
	}
	for _, group := range list(data["m_meshGroups"]) {
		group, _ := group.(string)
		out.Groups = append(out.Groups, group)
	}
	for _, m := range found {
		out.Meshes = append(out.Meshes, m.desc)
		out.Lods = max(out.Lods, uint32(bits.Len64(m.desc.LodMask)))
	}

	// Read each material the draw calls name once.
	for _, m := range found {
		for _, call := range m.desc.DrawCalls {
			if _, seen := out.Materials[call.Material]; call.Material != "" && !seen {
				out.Materials[call.Material] = a.material(call.Material)
			}
		}
	}
	writeMessage(w, out)
}

// material returns a material's shader and texture parameters, or an empty
// material when it cannot be read.
func (a *Assets) material(name string) *Material {
	out := &Material{Textures: map[string]string{}}
	_, blocks, err := a.read(name + "_c")
	if err != nil {
		return out
	}
	data, err := decodeTag(blocks, "DATA")
	if err != nil {
		return out
	}
	out.Shader, _ = data["m_shaderName"].(string)
	for _, item := range list(data["m_textureParams"]) {
		param, _ := item.(map[string]any)
		key, _ := param["m_name"].(string)
		if value, _ := param["m_pValue"].(string); key != "" && value != "" {
			out.Textures[key] = value + "_c"
		}
	}
	return out
}

// buffer answers one mesh buffer's bytes, meshopt-encoded when the model
// says so; the page decodes them. The mesh is its position in the Model and
// kind is "vertex" or "index".
func (a *Assets) buffer(w http.ResponseWriter, r *http.Request) {
	// Find the buffer by position.
	found, err := a.modelMeshes(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
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

	// Remove its zstd layer; meshopt stays for the page.
	data := buffers[i].data
	if buffers[i].zstd {
		if data, err = unzstd(data); err != nil {
			http.Error(w, err.Error(), http.StatusUnprocessableEntity)
			return
		}
	}
	writeBytes(w, "application/octet-stream", data)
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
	width, height, depth int
	format, mips, flags  int
	// pixels is where the mips start in the file, smallest mip first.
	pixels int
}

// cube is the texture flag of a cube map.
const cube = 4

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
		flags:  int(binary.LittleEndian.Uint16(d[2:])),
		width:  int(binary.LittleEndian.Uint16(d[20:])),
		height: int(binary.LittleEndian.Uint16(d[22:])),
		depth:  max(1, int(binary.LittleEndian.Uint16(d[24:]))),
		format: int(d[26]),
		mips:   max(1, int(d[27])),
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
	_, served := mipSize(header, 0)
	writeMessage(w, &Texture{
		Width:  uint32(header.width),
		Height: uint32(header.height),
		Depth:  uint32(header.depth),
		Format: uint32(header.format),
		Mips:   uint32(header.mips),
		Cube:   header.flags&cube != 0,
		Served: served,
	})
}

// mipSize returns the bytes one mip of the texture takes, and false for a
// format the page cannot upload as stored.
func mipSize(header textureHeader, mip int) (int, bool) {
	w, h := max(1, header.width>>mip), max(1, header.height>>mip)
	d := max(1, header.depth>>mip)
	if header.flags&cube != 0 {
		d *= 6
	}
	blocks := func(bytes int) int { return (w + 3) / 4 * ((h + 3) / 4) * bytes * d }
	switch header.format {
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
	// Read the header and check the mip and its format.
	mip, _ := strconv.Atoi(r.PathValue("mip"))
	data, header, err := a.readTexture(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	if mip < 0 || mip >= header.mips {
		http.Error(w, "no such mip", http.StatusNotFound)
		return
	}
	size, ok := mipSize(header, mip)
	if !ok {
		http.Error(w, "format "+strconv.Itoa(header.format)+" is not served yet", http.StatusUnprocessableEntity)
		return
	}

	// The mips are stored smallest first.
	at := header.pixels
	for smaller := header.mips - 1; smaller > mip; smaller-- {
		s, _ := mipSize(header, smaller)
		at += s
	}
	if at+size > len(data) {
		http.Error(w, "the texture is shorter than its mips", http.StatusUnprocessableEntity)
		return
	}
	writeBytes(w, "application/octet-stream", data[at:at+size])
}

// resource answers a resource's block tags and its KeyValues3 blocks as
// JSON.
func (a *Assets) resource(w http.ResponseWriter, r *http.Request) {
	// Decode every block that is KeyValues3.
	_, blocks, err := a.read(r.PathValue("name"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusNotFound)
		return
	}
	out := &Resource{}
	decoded := map[string]any{}
	for _, block := range blocks {
		out.Blocks = append(out.Blocks, block.Tag)
		if value, err := kv3.Decode(block.Data); err == nil {
			decoded[block.Tag] = value
		}
	}

	// Indent the JSON for reading.
	text, err := json.MarshalIndent(decoded, "", "  ")
	if err != nil {
		http.Error(w, err.Error(), http.StatusUnprocessableEntity)
		return
	}
	out.Json = string(text)
	writeMessage(w, out)
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

// writeMessage writes message as the response in the protobuf binary
// encoding.
func writeMessage(w http.ResponseWriter, message interface{ MarshalVT() ([]byte, error) }) {
	data, err := message.MarshalVT()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeBytes(w, "application/x-protobuf", data)
}

// writeBytes writes data as the response. The archive does not change while
// the server runs, so the browser may keep every answer.
func writeBytes(w http.ResponseWriter, contentType string, data []byte) {
	w.Header().Set("Content-Type", contentType)
	w.Header().Set("Cache-Control", "max-age=3600")
	_, _ = w.Write(data)
}
