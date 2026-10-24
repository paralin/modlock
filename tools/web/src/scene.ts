// Draws one game asset with three.js: a model with its color textures, or a
// texture on a plane. Every byte comes from the local server, which reads it
// from the player's own install; vertex and index buffers arrive
// meshopt-encoded and textures in their stored GPU block formats, so the
// browser decodes buffers and the GPU decodes texels.
import * as THREE from 'three'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import { MeshoptDecoder } from 'three/examples/jsm/libs/meshopt_decoder.module.js'

// api returns the server path for one asset route.
const api = (route: string, name: string) => `/api/assets/${route}/${name}`

// Shown is what a finished load reports to the page.
export interface Shown {
  // detail is one line about what is drawn, such as its size.
  detail: string
}

// TextureHeader is the fixed part of a texture's DATA block.
interface TextureHeader {
  width: number
  height: number
  depth: number
  format: number
  mips: number
  flags: number
}

// BufferDesc is one vertex or index buffer, the same for both layouts the
// game's models use.
interface BufferDesc {
  count: number
  stride: number
  meshopt: boolean
  fields:
    | { semantic: string; index: number; format: number; offset: number }[]
    | null
}

// DrawCall is one draw call of a mesh's scene object, as MDAT names it.
interface DrawCall {
  m_material: string
  m_nStartIndex: number
  m_nIndexCount: number
  m_nBaseVertex: number
  m_vertexBuffers: { m_hBuffer: number }[]
  m_indexBuffer: { m_hBuffer: number }
}

// ModelDesc is the server's answer for a model.
interface ModelDesc {
  meshes: {
    name: string
    vertexBuffers: BufferDesc[]
    indexBuffers: BufferDesc[]
    sceneObjects: { m_drawCalls: DrawCall[] | null }[] | null
  }[]
  materials: Record<
    string,
    { shader: string; textures: Record<string, string> } | null
  >
}

// fetchOk fetches a path and fails with the server's message.
async function fetchOk(path: string, signal?: AbortSignal): Promise<Response> {
  const response = await fetch(path, { signal })
  if (!response.ok)
    throw new Error((await response.text()).trim() || response.statusText)
  return response
}

// fetchJSON fetches a path's JSON answer.
export async function fetchJSON<T>(
  path: string,
  signal?: AbortSignal,
): Promise<T> {
  return (await (await fetchOk(path, signal)).json()) as T
}

// fetchBytes fetches a path's bytes.
async function fetchBytes(
  path: string,
  signal: AbortSignal,
): Promise<Uint8Array> {
  return new Uint8Array(await (await fetchOk(path, signal)).arrayBuffer())
}

// Viewer owns one canvas, its renderer and the asset drawn in it.
export class Viewer {
  private renderer: THREE.WebGLRenderer
  private scene = new THREE.Scene()
  private camera = new THREE.PerspectiveCamera(40, 1, 0.1, 100000)
  private controls: OrbitControls
  private content = new THREE.Group()
  private resize: ResizeObserver

  constructor(private canvas: HTMLCanvasElement) {
    // Draw at the screen's pixel density.
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true })
    this.renderer.setPixelRatio(window.devicePixelRatio)

    // Light the scene from the sky and one sun.
    this.scene.background = new THREE.Color(0x16181b)
    const sun = new THREE.DirectionalLight(0xffffff, 2)
    sun.position.set(1, 2, 1.5)
    this.scene.add(
      new THREE.HemisphereLight(0xffffff, 0x404048, 2.2),
      sun,
      this.content,
    )

    // Orbit with the pointer, follow the canvas's size and draw every frame.
    this.controls = new OrbitControls(this.camera, canvas)
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
    this.content.traverse((object) => {
      // Free the mesh's GPU buffers and texture.
      if (!(object instanceof THREE.Mesh)) return
      const mesh = object as THREE.Mesh<
        THREE.BufferGeometry,
        THREE.MeshStandardMaterial
      >
      mesh.geometry.dispose()
      mesh.material.map?.dispose()
      mesh.material.dispose()
    })
    this.content.clear()
    this.content.rotation.set(0, 0, 0)
  }

  // frame points the camera at the content's bounds from direction.
  private frame(direction = new THREE.Vector3(0.6, 0.35, 1)) {
    // Fit the bounds' diagonal in view.
    const box = new THREE.Box3().setFromObject(this.content)
    const size = box.getSize(new THREE.Vector3()).length() || 1
    const center = box.getCenter(new THREE.Vector3())
    this.controls.target.copy(center)
    this.camera.position
      .copy(center)
      .add(direction.normalize().multiplyScalar(size * 1.1))
    this.camera.near = size / 1000
    this.camera.far = size * 100
    this.camera.updateProjectionMatrix()
  }

  // showModel draws a model with each draw call's color texture. Textures
  // arrive after the model is drawn; signal ends the whole load.
  async showModel(name: string, signal: AbortSignal): Promise<Shown> {
    // Read the model's meshes and wait for the decoder.
    const desc = await fetchJSON<ModelDesc>(api('model', name), signal)
    if (desc.meshes.length === 0) throw new Error('This model has no meshes')
    await MeshoptDecoder.ready

    // Fetch and decode each buffer once, however many draw calls share it.
    const buffers = new Map<string, Promise<Uint8Array>>()
    const buffer = (
      mesh: number,
      kind: 'vertex' | 'index',
      index: number,
      desc: BufferDesc,
    ) => {
      const path = `/api/assets/buffer/${mesh}/${kind}/${index}/${name}`
      let decoded = buffers.get(path)
      if (!decoded) {
        decoded = decodeBuffer(path, kind, desc, signal)
        buffers.set(path, decoded)
      }
      return decoded
    }

    // Load each material's color texture once; a texture that fails leaves
    // its draw calls untextured.
    const textures = new Map<string, Promise<THREE.Texture | null>>()
    const colorOf = (material: string) => {
      // A material without a color texture stays grey.
      const path = desc.materials[material]?.textures.g_tColor
      if (!path) return Promise.resolve(null)
      let texture = textures.get(path)
      if (!texture) {
        texture = loadTexture(this.renderer, path, 1024, signal).catch(
          (error: Error) => {
            if (!signal.aborted) console.warn(`${path}: ${error.message}`)
            return null
          },
        )
        textures.set(path, texture)
      }
      return texture
    }

    // Build one mesh per draw call, textured when its color texture arrives.
    let vertices = 0
    let triangles = 0
    const meshes: THREE.Mesh[] = []
    for (const [m, mesh] of desc.meshes.entries()) {
      for (const object of mesh.sceneObjects ?? []) {
        for (const call of object.m_drawCalls ?? []) {
          const v = call.m_vertexBuffers[0]?.m_hBuffer ?? 0
          const i = call.m_indexBuffer.m_hBuffer
          const vb = mesh.vertexBuffers[v]
          const ib = mesh.indexBuffers[i]
          if (!vb || !ib) continue
          const [vertexData, indexData] = await Promise.all([
            buffer(m, 'vertex', v, vb),
            buffer(m, 'index', i, ib),
          ])
          vertices += vb.count
          triangles += call.m_nIndexCount / 3

          // Draw grey until the color texture arrives.
          const material = new THREE.MeshStandardMaterial({
            color: 0xbfbfbf,
            roughness: 0.8,
          })
          const drawn = new THREE.Mesh(
            buildGeometry(vb, vertexData, ib, indexData, call),
            material,
          )
          drawn.name = `${mesh.name}: ${call.m_material}`
          meshes.push(drawn)
          void colorOf(call.m_material).then((texture) => {
            if (!texture || signal.aborted) return
            material.map = texture
            material.color.set(0xffffff)
            material.needsUpdate = true
          })
        }
      }
    }

    // Replace what was drawn. Source 2 is Z up; three.js is Y up.
    signal.throwIfAborted()
    this.clear()
    this.content.add(...meshes)
    this.content.rotation.x = -Math.PI / 2
    this.frame()
    return {
      detail: `${meshes.length} draw calls, ${vertices.toLocaleString()} vertices, ${Math.round(triangles).toLocaleString()} triangles`,
    }
  }

  // showTexture draws a texture head on, on a plane at its aspect ratio.
  async showTexture(name: string, signal: AbortSignal): Promise<Shown> {
    // Read the size and the largest mip that fits 2048.
    const header = await fetchJSON<TextureHeader>(api('texture', name), signal)
    const texture = await loadTexture(this.renderer, name, 2048, signal)
    signal.throwIfAborted()

    // Replace what was drawn.
    const longest = Math.max(header.width, header.height)
    const plane = new THREE.Mesh(
      new THREE.PlaneGeometry(header.width / longest, header.height / longest),
      new THREE.MeshBasicMaterial({ map: texture, side: THREE.DoubleSide }),
    )
    this.clear()
    this.content.add(plane)
    this.frame(new THREE.Vector3(0, 0, 1))
    const image = texture.image as { width: number; height: number }
    return {
      detail: `${header.width}×${header.height} ${formatName(header.format)}, ${header.mips} mips, showing ${image.width}×${image.height}`,
    }
  }
}

// decodeBuffer fetches a mesh buffer and decodes it.
async function decodeBuffer(
  path: string,
  kind: 'vertex' | 'index',
  desc: BufferDesc,
  signal: AbortSignal,
): Promise<Uint8Array> {
  // Fetch the stored bytes; only meshopt buffers need decoding.
  const source = await fetchBytes(path, signal)
  if (!desc.meshopt) return source
  const target = new Uint8Array(desc.count * desc.stride)
  const decode =
    kind === 'vertex'
      ? MeshoptDecoder.decodeVertexBuffer
      : MeshoptDecoder.decodeIndexBuffer
  decode(target, desc.count, desc.stride, source)
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
  vb: BufferDesc,
  vertexData: Uint8Array,
  ib: BufferDesc,
  indexData: Uint8Array,
  call: DrawCall,
) {
  // Positions, texture coordinates and indices fill one geometry.
  const geometry = new THREE.BufferGeometry()

  // Unpack each attribute the page reads into floats.
  const view = new DataView(
    vertexData.buffer,
    vertexData.byteOffset,
    vertexData.byteLength,
  )
  const attribute = (semantic: string, target: string) => {
    // Find the first field of the semantic in a format the page reads.
    const field = vb.fields?.find(
      (f) => f.semantic === semantic && f.index === 0,
    )
    const reader = field && readers[field.format]
    if (!field || !reader) return
    const [size, read] = reader
    const out = new Float32Array(vb.count * size)
    for (let v = 0; v < vb.count; v++) {
      const at = v * vb.stride + field.offset
      for (let c = 0; c < size; c++) out[v * size + c] = read(view, at, c)
    }
    geometry.setAttribute(target, new THREE.BufferAttribute(out, size))
  }
  attribute('POSITION', 'position')
  attribute('TEXCOORD', 'uv')

  // Take the draw call's indices, rebased on its first vertex. A buffer read
  // in place may sit at an unaligned offset, so copy it out first.
  const bytes =
    indexData.byteOffset % ib.stride === 0 ? indexData : indexData.slice()
  const all =
    ib.stride === 4
      ? new Uint32Array(bytes.buffer, bytes.byteOffset, ib.count)
      : new Uint16Array(bytes.buffer, bytes.byteOffset, ib.count)
  const slice = new Uint32Array(call.m_nIndexCount)
  for (let i = 0; i < call.m_nIndexCount; i++)
    slice[i] = all[call.m_nStartIndex + i] + call.m_nBaseVertex
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
const formatName = (format: number) => formatNames[format] ?? `format ${format}`

// loadTexture loads the largest mip of a texture no wider than limit.
async function loadTexture(
  renderer: THREE.WebGLRenderer,
  name: string,
  limit: number,
  signal: AbortSignal,
): Promise<THREE.Texture> {
  // Pick the mip and fetch its stored bytes.
  const header = await fetchJSON<TextureHeader>(api('texture', name), signal)
  let mip = 0
  while (
    mip < header.mips - 1 &&
    Math.max(header.width >> mip, header.height >> mip) > limit
  )
    mip++
  const width = Math.max(1, header.width >> mip)
  const height = Math.max(1, header.height >> mip)
  const data = await fetchBytes(`/api/assets/mip/${mip}/${name}`, signal)

  // Upload block formats as stored and swizzle BGRA into RGBA.
  let texture: THREE.Texture
  const block = compressed[header.format]
  if (block) {
    if (!renderer.getContext().getExtension(block[1])) {
      throw new Error(
        `This browser's GPU cannot draw ${formatName(header.format)} textures (${block[1]})`,
      )
    }
    texture = new THREE.CompressedTexture(
      [{ data, width, height }],
      width,
      height,
      block[0],
    )
  } else if (header.format === 4 || header.format === 28) {
    if (header.format === 28) {
      for (let i = 0; i < data.length; i += 4)
        [data[i], data[i + 2]] = [data[i + 2], data[i]]
    }
    texture = new THREE.DataTexture(
      data.subarray(0, width * height * 4),
      width,
      height,
      THREE.RGBAFormat,
    )
  } else {
    throw new Error(`${formatName(header.format)} textures are not drawn yet`)
  }

  // Sample the one mip as is; one and two channel formats hold data, not
  // color.
  texture.minFilter = THREE.LinearFilter
  texture.magFilter = THREE.LinearFilter
  texture.generateMipmaps = false
  texture.flipY = false
  if (header.format !== 21 && header.format !== 27)
    texture.colorSpace = THREE.SRGBColorSpace
  texture.needsUpdate = true
  return texture
}
