// Draws one game asset with three.js: a model with its color textures, or a
// texture on a plane. Every byte comes from the local server, which reads it
// from the player's own install; vertex and index buffers arrive
// meshopt-encoded and textures in their stored GPU block formats, so the
// browser decodes buffers and the GPU decodes texels.
import * as THREE from 'three'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import { MeshoptDecoder } from 'three/examples/jsm/libs/meshopt_decoder.module.js'
import {
  Texture,
  type Buffer,
  type DrawCall,
  type Model,
} from '../studio.pb.js'
import { fetchBytes, fetchMessage, route } from './api.js'

// Shading is how the viewport draws a model's surfaces.
export type Shading = 'material' | 'solid' | 'wireframe'

// Stats counts what the viewport draws.
export interface Stats {
  drawCalls: number
  vertices: number
  triangles: number
}

// Unavailable is a texture this browser or server cannot draw, as opposed to
// one that failed to load.
export class Unavailable extends Error {}

// Selection picks which of a model's meshes draw: one level of detail and
// the mesh groups in a mask, or every group when the model names none.
export interface Selection {
  lod: number
  groups: bigint | null
}

// LoadedModel is the model the viewport holds and the draw calls it has
// built so far, by mesh and call index.
interface LoadedModel {
  name: string
  model: Model
  signal: AbortSignal
  built: Map<string, Promise<THREE.Mesh | null>>
  buffers: Map<string, Promise<Uint8Array>>
  textures: Map<string, Promise<THREE.Texture | null>>
  framed: boolean
}

// warmGrey is the solid shading's surface color.
const warmGrey = 0xc9c1b4

// Viewer owns one canvas, its renderer and the asset drawn in it.
export class Viewer {
  private renderer: THREE.WebGLRenderer
  private scene = new THREE.Scene()
  private camera = new THREE.PerspectiveCamera(40, 1, 0.1, 100000)
  private controls: OrbitControls
  private content = new THREE.Group()
  private grid: THREE.GridHelper | null = null
  private gridShown = true
  private shading: Shading = 'material'
  private loaded: LoadedModel | null = null
  private resize: ResizeObserver

  // onMissing hears each texture of the drawn model that cannot be drawn.
  constructor(
    private canvas: HTMLCanvasElement,
    private onMissing: (texture: string, reason: string) => void,
  ) {
    // Draw at the screen's pixel density over the page's own background.
    this.renderer = new THREE.WebGLRenderer({
      canvas,
      antialias: true,
      alpha: true,
    })
    this.renderer.setPixelRatio(window.devicePixelRatio)

    // Light the scene from the sky and one sun.
    const sun = new THREE.DirectionalLight(0xfff4e6, 2)
    sun.position.set(1, 2, 1.5)
    this.scene.add(
      new THREE.HemisphereLight(0xfff4e6, 0x3a3128, 2.2),
      sun,
      this.content,
    )

    // Orbit with the pointer, follow the canvas's size and draw every frame.
    this.controls = new OrbitControls(this.camera, canvas)
    this.controls.enableDamping = true
    this.resize = new ResizeObserver(() => this.fit())
    this.resize.observe(canvas.parentElement!)
    this.renderer.setAnimationLoop(() => {
      this.controls.update()
      this.renderer.render(this.scene, this.camera)
    })
  }

  // dispose releases the renderer.
  dispose() {
    this.resize.disconnect()
    this.renderer.setAnimationLoop(null)
    this.clear()
    this.renderer.dispose()
  }

  // fit sizes the drawing buffer to the canvas's box.
  private fit() {
    const box = this.canvas.parentElement!.getBoundingClientRect()
    this.renderer.setSize(box.width, box.height, false)
    this.camera.aspect = box.width / Math.max(1, box.height)
    this.camera.updateProjectionMatrix()
  }

  // clear removes and frees the drawn asset.
  clear() {
    // Free every mesh, then drop them and the grid.
    this.content.traverse((object) => {
      // Free the mesh's GPU buffers and textures.
      if (!(object instanceof THREE.Mesh)) return
      const mesh = object as THREE.Mesh<
        THREE.BufferGeometry,
        THREE.MeshStandardMaterial
      >
      mesh.geometry.dispose()
      ;(mesh.userData.map as THREE.Texture | undefined)?.dispose()
      mesh.material.map?.dispose()
      mesh.material.dispose()
    })
    this.content.clear()
    this.content.rotation.set(0, 0, 0)
    this.loaded = null
    this.setGridSize(null)
  }

  // setShading draws every surface in the given shading.
  setShading(shading: Shading) {
    this.shading = shading
    this.content.traverse((object) => {
      if (object instanceof THREE.Mesh && object.userData.model)
        this.shade(
          object as THREE.Mesh<
            THREE.BufferGeometry,
            THREE.MeshStandardMaterial
          >,
        )
    })
  }

  // shade applies the shading to one of a model's meshes.
  private shade(
    mesh: THREE.Mesh<THREE.BufferGeometry, THREE.MeshStandardMaterial>,
  ) {
    // Material shading draws the color texture; the others draw grey.
    const map =
      this.shading === 'material'
        ? ((mesh.userData.map as THREE.Texture | undefined) ?? null)
        : null
    mesh.material.map = map
    mesh.material.color.set(map ? 0xffffff : warmGrey)
    mesh.material.wireframe = this.shading === 'wireframe'
    mesh.material.needsUpdate = true
  }

  // setGrid shows or hides the floor grid.
  setGrid(shown: boolean) {
    this.gridShown = shown
    if (this.grid) this.grid.visible = shown
  }

  // setGridSize places a floor grid under a box, or removes it.
  private setGridSize(box: THREE.Box3 | null) {
    // Remove the old grid.
    if (this.grid) {
      this.scene.remove(this.grid)
      this.grid.dispose()
      this.grid = null
    }
    if (!box) return

    // Size the grid in powers of ten around the box's footprint.
    const size = box.getSize(new THREE.Vector3())
    const span = Math.max(size.x, size.z, 1)
    const cell = 10 ** Math.floor(Math.log10(span / 4))
    const extent = Math.ceil((span * 3) / cell) * cell
    this.grid = new THREE.GridHelper(
      extent,
      Math.round(extent / cell),
      0x6b5a48,
      0x2e2822,
    )

    // Lay it under the model's lowest point.
    const center = box.getCenter(new THREE.Vector3())
    this.grid.position.set(center.x, box.min.y, center.z)
    this.grid.visible = this.gridShown
    this.scene.add(this.grid)
  }

  // frame points the camera at the content's bounds from direction.
  frame(direction = new THREE.Vector3(0.6, 0.35, 1)) {
    // Find the visible meshes' bounding sphere.
    const box = new THREE.Box3()
    this.content.traverseVisible((object) => {
      if (object instanceof THREE.Mesh) box.expandByObject(object)
    })
    if (box.isEmpty()) return
    const size = box.getSize(new THREE.Vector3()).length() || 1
    const center = box.getCenter(new THREE.Vector3())

    // Back off until the sphere fits the narrower of the two fields of view,
    // with a margin.
    const vertical = THREE.MathUtils.degToRad(this.camera.fov) / 2
    const horizontal = Math.atan(Math.tan(vertical) * this.camera.aspect)
    const distance =
      (size / 2 / Math.sin(Math.min(vertical, horizontal))) * 1.05
    this.controls.target.copy(center)
    this.camera.position
      .copy(center)
      .add(direction.normalize().multiplyScalar(distance))
    this.camera.near = size / 1000
    this.camera.far = size * 100
    this.camera.updateProjectionMatrix()
  }

  // loadModel replaces what is drawn with a model, drawing nothing until
  // showModel picks its meshes. signal ends every load the model starts.
  async loadModel(name: string, model: Model, signal: AbortSignal) {
    // Replace the drawn asset once the buffer decoder is ready.
    await MeshoptDecoder.ready
    signal.throwIfAborted()
    this.clear()
    this.loaded = {
      name,
      model,
      signal,
      built: new Map(),
      buffers: new Map(),
      textures: new Map(),
      framed: false,
    }

    // Source 2 is Z up; three.js is Y up.
    this.content.rotation.x = -Math.PI / 2
  }

  // showModel draws the loaded model's meshes the selection picks, building
  // each draw call the first time it shows. It frames the model the first
  // time. Textures arrive after the draw calls.
  async showModel(selection: Selection): Promise<Stats> {
    // Build every draw call the selection shows.
    const loaded = this.loaded
    if (!loaded) throw new Error('No model is loaded')
    const shown: Promise<THREE.Mesh | null>[] = []
    const hidden: Promise<THREE.Mesh | null>[] = []
    for (const [m, mesh] of (loaded.model.meshes ?? []).entries()) {
      const visible =
        (((mesh.lodMask ?? 1n) >> BigInt(selection.lod)) & 1n) !== 0n &&
        (selection.groups === null ||
          ((mesh.groupMask ?? 0n) & selection.groups) !== 0n)
      for (const [c, call] of (mesh.drawCalls ?? []).entries()) {
        const key = `${m}/${c}`
        let built = loaded.built.get(key)
        if (!built && visible) {
          built = this.buildCall(loaded, m, call)
          loaded.built.set(key, built)
        }
        if (built) (visible ? shown : hidden).push(built)
      }
    }

    // Show only those calls, counting what they draw; calls that share a
    // vertex buffer count its vertices once.
    const [drawn, undrawn] = await Promise.all([
      Promise.all(shown),
      Promise.all(hidden),
    ])
    if (this.loaded !== loaded) throw new DOMException('replaced', 'AbortError')
    const stats: Stats = { drawCalls: 0, vertices: 0, triangles: 0 }
    const counted = new Set<string>()
    for (const mesh of undrawn) if (mesh) mesh.visible = false
    for (const mesh of drawn) {
      if (!mesh) continue
      mesh.visible = true
      stats.drawCalls++
      stats.triangles += (mesh.geometry.index?.count ?? 0) / 3
      const vertexBuffer = mesh.userData.vertexBuffer as string
      if (counted.has(vertexBuffer)) continue
      counted.add(vertexBuffer)
      stats.vertices += mesh.geometry.attributes.position?.count ?? 0
    }

    // Frame the first view and put the grid under it.
    if (!loaded.framed && stats.drawCalls > 0) {
      loaded.framed = true
      this.content.updateMatrixWorld()
      this.frame()
      this.setGridSize(new THREE.Box3().setFromObject(this.content))
    }
    return stats
  }

  // buildCall builds one draw call's mesh and adds it hidden, or returns
  // null for a call whose buffers are missing.
  private async buildCall(
    loaded: LoadedModel,
    m: number,
    call: DrawCall,
  ): Promise<THREE.Mesh | null> {
    // Fetch and decode its buffers, shared with the mesh's other calls.
    const mesh = loaded.model.meshes![m]!
    const v = call.vertexBuffer ?? 0
    const i = call.indexBuffer ?? 0
    const vb = mesh.vertexBuffers?.[v]
    const ib = mesh.indexBuffers?.[i]
    if (!vb || !ib) return null
    const [vertexData, indexData] = await Promise.all([
      this.buffer(loaded, m, 'vertex', v, vb),
      this.buffer(loaded, m, 'index', i, ib),
    ])
    loaded.signal.throwIfAborted()

    // Draw grey until the color texture arrives.
    const drawn = new THREE.Mesh(
      buildGeometry(vb, vertexData, ib, indexData, call),
      new THREE.MeshStandardMaterial({ color: warmGrey, roughness: 0.8 }),
    )

    // Add it hidden; showModel decides what shows.
    drawn.name = `${mesh.name ?? ''}: ${call.material ?? ''}`
    drawn.userData.model = true
    drawn.userData.vertexBuffer = `${m}/${v}`
    drawn.visible = false
    this.shade(drawn)
    this.content.add(drawn)

    // Swap in the color texture when it arrives.
    void this.colorOf(loaded, call.material ?? '').then((texture) => {
      if (!texture || this.loaded !== loaded) return
      drawn.userData.map = texture
      this.shade(drawn)
    })
    return drawn
  }

  // buffer fetches and decodes one buffer of a mesh once.
  private buffer(
    loaded: LoadedModel,
    mesh: number,
    kind: 'vertex' | 'index',
    index: number,
    desc: Buffer,
  ) {
    const path = route(`buffer/${mesh}/${kind}/${index}`, loaded.name)
    let decoded = loaded.buffers.get(path)
    if (!decoded) {
      decoded = decodeBuffer(path, kind, desc, loaded.signal)
      loaded.buffers.set(path, decoded)
    }
    return decoded
  }

  // colorOf loads a material's color texture once; one that cannot be drawn
  // leaves its draw calls grey and is reported.
  private colorOf(loaded: LoadedModel, material: string) {
    // A material without a color texture stays grey.
    const path = loaded.model.materials?.[material]?.textures?.g_tColor
    if (!path) return Promise.resolve(null)
    let texture = loaded.textures.get(path)
    if (!texture) {
      texture = loadTexture(this.renderer, path, null, 1024, loaded.signal)
        .then(({ texture }) => texture)
        .catch((error: Error) => {
          if (!loaded.signal.aborted) this.onMissing(path, error.message)
          return null
        })
      loaded.textures.set(path, texture)
    }
    return texture
  }

  // showTexture draws a texture head on, on a plane at its aspect ratio, and
  // returns the size of the mip it shows.
  async showTexture(
    name: string,
    header: Texture,
    signal: AbortSignal,
  ): Promise<{ width: number; height: number }> {
    // Read the largest mip that fits 2048.
    const { texture, width, height } = await loadTexture(
      this.renderer,
      name,
      header,
      2048,
      signal,
    )
    signal.throwIfAborted()

    // Replace what was drawn.
    const w = header.width ?? 1
    const h = header.height ?? 1
    const longest = Math.max(w, h)
    const plane = new THREE.Mesh(
      new THREE.PlaneGeometry(w / longest, h / longest),
      new THREE.MeshBasicMaterial({ map: texture, side: THREE.DoubleSide }),
    )
    this.clear()
    this.content.add(plane)
    this.frame(new THREE.Vector3(0, 0, 1))
    return { width, height }
  }
}

// decodeBuffer fetches a mesh buffer and decodes it.
async function decodeBuffer(
  path: string,
  kind: 'vertex' | 'index',
  desc: Buffer,
  signal: AbortSignal,
): Promise<Uint8Array> {
  // Fetch the stored bytes; only meshopt buffers need decoding.
  const source = await fetchBytes(path, signal)
  if (!desc.meshopt) return source
  const count = desc.count ?? 0
  const stride = desc.stride ?? 0
  const target = new Uint8Array(count * stride)
  const decode =
    kind === 'vertex'
      ? MeshoptDecoder.decodeVertexBuffer
      : MeshoptDecoder.decodeIndexBuffer
  decode(target, count, stride, source)
  return target
}

// readers turns one DXGI vertex format into its component count and a
// function reading component c of the value at a byte offset.
const readers: Record<
  number,
  [number, (view: DataView, at: number, c: number) => number]
> = {
  2: [4, (v, at, c) => v.getFloat32(at + 4 * c, true)], // R32G32B32A32_FLOAT
  6: [3, (v, at, c) => v.getFloat32(at + 4 * c, true)], // R32G32B32_FLOAT
  16: [2, (v, at, c) => v.getFloat32(at + 4 * c, true)], // R32G32_FLOAT
  34: [
    2,
    (v, at, c) => THREE.DataUtils.fromHalfFloat(v.getUint16(at + 2 * c, true)),
  ], // R16G16_FLOAT
  35: [2, (v, at, c) => v.getUint16(at + 2 * c, true) / 65535], // R16G16_UNORM
  37: [2, (v, at, c) => Math.max(-1, v.getInt16(at + 2 * c, true) / 32767)], // R16G16_SNORM
}

// buildGeometry makes one draw call's geometry from its decoded buffers:
// positions and the first texture coordinates, with normals computed.
function buildGeometry(
  vb: Buffer,
  vertexData: Uint8Array,
  ib: Buffer,
  indexData: Uint8Array,
  call: DrawCall,
) {
  // Positions, texture coordinates and indices fill one geometry.
  const geometry = new THREE.BufferGeometry()
  const count = vb.count ?? 0
  const stride = vb.stride ?? 0

  // Unpack each attribute the page reads into floats.
  const view = new DataView(
    vertexData.buffer,
    vertexData.byteOffset,
    vertexData.byteLength,
  )
  const attribute = (semantic: string, target: string) => {
    // Find the first field of the semantic in a format the page reads.
    const field = vb.fields?.find(
      (f) => f.semantic === semantic && (f.index ?? 0) === 0,
    )
    const reader = field && readers[field.format ?? 0]
    if (!field || !reader) return
    const [size, read] = reader
    const out = new Float32Array(count * size)
    for (let v = 0; v < count; v++) {
      const at = v * stride + (field.offset ?? 0)
      for (let c = 0; c < size; c++) out[v * size + c] = read(view, at, c)
    }
    geometry.setAttribute(target, new THREE.BufferAttribute(out, size))
  }
  attribute('POSITION', 'position')
  attribute('TEXCOORD', 'uv')

  // Take the draw call's indices, rebased on its first vertex. A buffer read
  // in place may sit at an unaligned offset, so copy it out first.
  const indexStride = ib.stride ?? 2
  const bytes =
    indexData.byteOffset % indexStride === 0 ? indexData : indexData.slice()
  const all =
    indexStride === 4
      ? new Uint32Array(bytes.buffer, bytes.byteOffset, ib.count ?? 0)
      : new Uint16Array(bytes.buffer, bytes.byteOffset, ib.count ?? 0)
  const start = call.startIndex ?? 0
  const base = call.baseVertex ?? 0
  const slice = new Uint32Array(call.indexCount ?? 0)
  for (let i = 0; i < slice.length; i++) slice[i] = all[start + i]! + base
  geometry.setIndex(new THREE.BufferAttribute(slice, 1))

  // The stored normals are a packed tangent frame; compute them instead.
  geometry.computeVertexNormals()
  return geometry
}

// compressed maps a block-compressed texture format to the three.js format
// it uploads as unchanged and the WebGL extension that draws it.
const compressed: Record<number, [THREE.CompressedPixelFormat, string]> = {
  1: [THREE.RGBA_S3TC_DXT1_Format, 'WEBGL_compressed_texture_s3tc'],
  2: [THREE.RGBA_S3TC_DXT5_Format, 'WEBGL_compressed_texture_s3tc'],
  19: [THREE.RGB_BPTC_UNSIGNED_Format, 'EXT_texture_compression_bptc'],
  20: [THREE.RGBA_BPTC_Format, 'EXT_texture_compression_bptc'],
  21: [THREE.RED_GREEN_RGTC2_Format, 'EXT_texture_compression_rgtc'],
  27: [THREE.RED_RGTC1_Format, 'EXT_texture_compression_rgtc'],
}

// formatNames names the texture formats the page knows.
const formatNames: Record<number, string> = {
  1: 'DXT1',
  2: 'DXT5',
  4: 'RGBA8888',
  19: 'BC6H',
  20: 'BC7',
  21: 'BC5',
  27: 'BC4',
  28: 'BGRA8888',
}

// formatName names a texture format.
export const formatName = (format: number) =>
  formatNames[format] ?? `Format ${format}`

// loadTexture loads the largest mip of a texture no wider than limit,
// reading its header first when header is null.
async function loadTexture(
  renderer: THREE.WebGLRenderer,
  name: string,
  header: Texture | null,
  limit: number,
  signal: AbortSignal,
): Promise<{ texture: THREE.Texture; width: number; height: number }> {
  // Check that the format can be drawn before fetching any texels.
  header ??= await fetchMessage(Texture, route('texture', name), signal)
  const format = header.format ?? 0
  const block = compressed[format]
  if (!header.served)
    throw new Unavailable(`${formatName(format)} textures are not served yet`)
  if (block && !renderer.getContext().getExtension(block[1]))
    throw new Unavailable(
      `This browser's GPU cannot draw ${formatName(format)} textures`,
    )

  // Pick the mip and fetch its stored bytes.
  const mips = header.mips ?? 1
  let mip = 0
  while (
    mip < mips - 1 &&
    Math.max((header.width ?? 0) >> mip, (header.height ?? 0) >> mip) > limit
  )
    mip++
  const width = Math.max(1, (header.width ?? 0) >> mip)
  const height = Math.max(1, (header.height ?? 0) >> mip)
  const data = await fetchBytes(route(`mip/${mip}`, name), signal)

  // Upload block formats as stored and swizzle BGRA into RGBA.
  let texture: THREE.Texture
  if (block) {
    texture = new THREE.CompressedTexture(
      [{ data, width, height }],
      width,
      height,
      block[0],
    )
  } else {
    if (format === 28) {
      for (let i = 0; i < data.length; i += 4)
        [data[i], data[i + 2]] = [data[i + 2]!, data[i]!]
    }
    texture = new THREE.DataTexture(
      data.subarray(0, width * height * 4),
      width,
      height,
      THREE.RGBAFormat,
    )
  }

  // Sample the one mip as is; one and two channel formats hold data, not
  // color.
  texture.minFilter = THREE.LinearFilter
  texture.magFilter = THREE.LinearFilter
  texture.generateMipmaps = false
  texture.flipY = false
  if (format !== 21 && format !== 27) texture.colorSpace = THREE.SRGBColorSpace
  texture.needsUpdate = true
  return { texture, width, height }
}
