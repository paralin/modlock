// Builds three.js geometry from a model's stored vertex and index buffers,
// which arrive meshopt-encoded with each attribute in its DXGI format.
import * as THREE from 'three'
import { MeshoptDecoder } from 'three/examples/jsm/libs/meshopt_decoder.module.js'
import type { Buffer, DrawCall } from '../studio.pb.js'
import { fetchBytes } from './api.js'

// decodeBuffer fetches a mesh buffer and decodes it.
export async function decodeBuffer(
  path: string,
  kind: 'vertex' | 'index',
  desc: Buffer,
  signal: AbortSignal,
): Promise<Uint8Array> {
  // Fetch the stored bytes; only meshopt buffers need decoding.
  const [source] = await Promise.all([
    fetchBytes(path, signal),
    MeshoptDecoder.ready,
  ])
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

// Layout is how one DXGI vertex format is copied out into an attribute: its
// component count and size, the typed array that holds the components as
// stored, and whether they read as normalized integers.
interface Layout {
  size: number
  bytes: number
  array: new (length: number) => Float32Array | Uint16Array | Int16Array
  normalized: boolean
  half?: boolean
}

// layouts are the vertex formats the page reads, kept in their stored
// precision so the GPU gets them unconverted.
const layouts: Record<number, Layout> = {
  2: { size: 4, bytes: 4, array: Float32Array, normalized: false }, // R32G32B32A32_FLOAT
  6: { size: 3, bytes: 4, array: Float32Array, normalized: false }, // R32G32B32_FLOAT
  16: { size: 2, bytes: 4, array: Float32Array, normalized: false }, // R32G32_FLOAT
  34: { size: 2, bytes: 2, array: Uint16Array, normalized: false, half: true }, // R16G16_FLOAT
  35: { size: 2, bytes: 2, array: Uint16Array, normalized: true }, // R16G16_UNORM
  37: { size: 2, bytes: 2, array: Int16Array, normalized: true }, // R16G16_SNORM
}

// packedFrame is the R32_UINT format of a vertex's whole tangent frame.
const packedFrame = 42

// rgba8 is the R8G8B8A8_UNORM format of a vertex color.
const rgba8 = 28

// Field is where one attribute sits: its stream's bytes, vertex count and
// stride, and its offset in each vertex.
interface Field {
  view: DataView
  bytes: Uint8Array
  count: number
  stride: number
  at: number
  format: number
}

// Stream is one vertex buffer a draw call reads, with its decoded bytes.
export interface Stream {
  buffer: Buffer
  data: Uint8Array
}

// Vertices are the attributes of one set of streams, shared by every draw
// call that reads the same set, with the bounds of all their vertices.
export interface Vertices {
  attributes: Record<string, THREE.BufferAttribute>
  box: THREE.Box3
  sphere: THREE.Sphere
  // framed is whether the streams store a tangent frame; without one each
  // call computes its own normals from the positions.
  framed: boolean
}

// readVertices copies out the attributes the page draws from a draw call's
// streams: positions, the first texture coordinates, the vertex color and
// the stored tangent frame. Each attribute may sit in any stream.
export function readVertices(streams: Stream[]): Vertices {
  // Find a semantic's first field and the stream that holds it.
  const field = (semantic: string): Field | null => {
    for (const { buffer, data } of streams) {
      const found = buffer.fields?.find(
        (f) => f.semantic === semantic && (f.index ?? 0) === 0,
      )
      if (!found) continue
      return {
        view: new DataView(data.buffer, data.byteOffset, data.byteLength),
        bytes: data,
        count: buffer.count ?? 0,
        stride: buffer.stride ?? 0,
        at: found.offset ?? 0,
        format: found.format ?? 0,
      }
    }
    return null
  }

  // Copy each attribute out of its stream in its stored format.
  const attributes: Record<string, THREE.BufferAttribute> = {}
  const copy = (semantic: string, target: string) => {
    // Skip a field the mesh lacks or stores in an unknown format.
    const f = field(semantic)
    const layout = f && layouts[f.format]
    if (!f || !layout) return

    // Gather each vertex's bytes into a tight array.
    const { size, bytes, half, normalized } = layout
    const out = new layout.array(f.count * size)
    const target8 = new Uint8Array(out.buffer)
    const width = size * bytes
    for (let v = 0; v < f.count; v++) {
      const at = f.bytes.byteOffset + v * f.stride + f.at
      target8.set(new Uint8Array(f.bytes.buffer, at, width), v * width)
    }

    // Wrap it as the attribute three.js reads.
    attributes[target] = half
      ? new THREE.Float16BufferAttribute(out, size)
      : new THREE.BufferAttribute(out, size, normalized)
  }
  copy('POSITION', 'position')
  copy('TEXCOORD', 'uv')

  // Read the vertex color, stored as sRGB bytes, as a linear color.
  const color = field('COLOR')
  if (color?.format === rgba8) {
    const out = new Uint16Array(color.count * 3)
    const c = new THREE.Color()
    for (let v = 0; v < color.count; v++) {
      const at = v * color.stride + color.at
      const byte = (i: number) => color.view.getUint8(at + i) / 255
      c.setRGB(byte(0), byte(1), byte(2), THREE.SRGBColorSpace)
      out[v * 3] = c.r * 65535
      out[v * 3 + 1] = c.g * 65535
      out[v * 3 + 2] = c.b * 65535
    }
    attributes.color = new THREE.BufferAttribute(out, 3, true)
  }

  // Unpack the stored tangent frame.
  const normal = field('NORMAL')
  const framed = normal?.format === packedFrame
  if (framed) Object.assign(attributes, unpackFrames(normal))

  // Bound every vertex once for all the calls that share them.
  const box = new THREE.Box3()
  if (attributes.position) box.setFromBufferAttribute(attributes.position)
  const sphere = box.getBoundingSphere(new THREE.Sphere())

  // Free each copy once it reaches the GPU; positions stay while calls may
  // still compute normals from them.
  for (const [name, attribute] of Object.entries(attributes))
    if (framed || name !== 'position')
      attribute.onUpload(function (this: THREE.BufferAttribute) {
        ;(this as { array: unknown }).array = null
      })
  return { attributes, box, sphere, framed }
}

// buildGeometry makes one draw call's geometry over its shared vertices,
// computing normals when the streams store no tangent frame.
export function buildGeometry(
  vertices: Vertices,
  ib: Buffer,
  indexData: Uint8Array,
  call: DrawCall,
) {
  // Share the vertices and their bounds.
  const geometry = new THREE.BufferGeometry()
  for (const [name, attribute] of Object.entries(vertices.attributes))
    geometry.setAttribute(name, attribute)
  geometry.boundingBox = vertices.box.clone()
  geometry.boundingSphere = vertices.sphere.clone()

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

  // Compute normals when no frame is stored.
  if (!vertices.framed) geometry.computeVertexNormals()
  return geometry
}

// unpackFrames reads every vertex's packed tangent frame into normal and
// tangent attributes. Each 32-bit value holds the normal as a 10:10
// octahedral pair, the tangent as an 11-bit angle around it from a basis the
// normal fixes, and the bitangent's sign in the low bit.
function unpackFrames({ view, count, stride, at }: Field) {
  // Fill a normal and a signed tangent for each vertex, as 16-bit
  // normalized integers.
  const normals = new Int16Array(count * 3)
  const tangents = new Int16Array(count * 4)
  const n = new THREE.Vector3()
  const u = new THREE.Vector3()
  const t = new THREE.Vector3()
  for (let v = 0; v < count; v++) {
    // Unfold the octahedral normal.
    const packed = view.getUint32(v * stride + at, true)
    let x = (((packed >>> 12) & 0x3ff) / 1023) * 2 - 1
    let y = (((packed >>> 22) & 0x3ff) / 1023) * 2 - 1
    const z = 1 - Math.abs(x) - Math.abs(y)
    const fold = Math.max(0, -z)
    x += x >= 0 ? -fold : fold
    y += y >= 0 ? -fold : fold
    n.set(x, y, z).normalize()

    // Turn the basis tangent around the normal by the stored angle.
    const side = n.z >= 0 ? 1 : -1
    const k = 1 / (side + n.z)
    u.set(-side * n.x * n.x * k + 1, -side * n.x * n.y * k, -side * n.x)
    const angle = (((packed >>> 1) & 0x7ff) / 2047) * Math.PI * 2
    t.crossVectors(n, u).multiplyScalar(Math.sin(angle))
    t.addScaledVector(u, Math.cos(angle))

    // Store them, with the bitangent's sign in the tangent's w.
    normals[v * 3] = n.x * 32767
    normals[v * 3 + 1] = n.y * 32767
    normals[v * 3 + 2] = n.z * 32767
    tangents[v * 4] = t.x * 32767
    tangents[v * 4 + 1] = t.y * 32767
    tangents[v * 4 + 2] = t.z * 32767
    tangents[v * 4 + 3] = packed & 1 ? 32767 : -32767
  }
  return {
    normal: new THREE.BufferAttribute(normals, 3, true),
    tangent: new THREE.BufferAttribute(tangents, 4, true),
  }
}
