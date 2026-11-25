// Draws the server's render job: each shot's model alone on a transparent
// ground, framed by its bounds and lit by a rig that turns with the camera,
// so a set of models reads as one. Each picture is trimmed to the model with
// an even margin and sent back as a PNG with straight alpha.
import * as THREE from 'three'
import { RoomEnvironment } from 'three/examples/jsm/environments/RoomEnvironment.js'
import { Model, RenderJob, ShotResult, type Shot } from '../studio.pb.js'
import { fetchMessage, route } from './api.js'
import { calls, Materials, Parts } from './models.js'
import { defaultGroups } from './state.js'
import { TextureCache } from './textures.js'

// A shot is drawn at least this many times its final size, so the
// downscale smooths every edge.
const supersample = 2

// The largest square drawn for one shot, in pixels.
const largest = 8192

// A shot that has not finished in this long fails, so one bad model cannot
// stall the job.
const shotTimeout = 120_000

// Loaded is the model on the stage and what it drew without.
interface Loaded {
  name: string
  group: THREE.Group
  materials: Materials
  missing: string[]
}

// Stage owns the renderer, the light rig and the model last drawn, which
// the next shot of the same model reuses.
class Stage {
  private canvas = document.createElement('canvas')
  private renderer = new THREE.WebGLRenderer({
    canvas: this.canvas,
    antialias: true,
    alpha: true,
    preserveDrawingBuffer: true,
  })
  private scene = new THREE.Scene()
  private camera = new THREE.PerspectiveCamera()
  private key = new THREE.DirectionalLight(0xfff4e6, 1.8)
  private rim = new THREE.DirectionalLight(0xe6eeff, 1.2)
  private loaded: Loaded | null = null

  constructor() {
    // Tone map as the browser's viewport does, over a clear ground.
    this.renderer.setPixelRatio(1)
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping
    this.renderer.setClearColor(0x000000, 0)

    // Light from the viewport's soft studio room for fill and reflections,
    // a warm key and a cool rim.
    const pmrem = new THREE.PMREMGenerator(this.renderer)
    this.scene.environment = pmrem.fromScene(
      new RoomEnvironment(),
      0.04,
    ).texture
    this.scene.environmentIntensity = 0.55
    pmrem.dispose()
    this.scene.add(this.key, this.key.target, this.rim, this.rim.target)
  }

  // shoot draws one shot and returns its answer.
  async shoot(shot: Shot, signal: AbortSignal): Promise<ShotResult> {
    // Load the model unless the last shot drew it.
    const name = shot.model ?? ''
    if (this.loaded?.name !== name) this.loaded = await this.load(name, signal)
    const { group, missing } = this.loaded

    // Apply each shot's model tint independently of the previous shot's tint.
    const [r = 1, g = 1, b = 1] = shot.tint ?? []
    const tint = new THREE.Color().setRGB(r, g, b, THREE.SRGBColorSpace)
    group.traverse((object) => {
      if (!(object instanceof THREE.Mesh)) return
      const materials = Array.isArray(object.material)
        ? object.material
        : [object.material]
      for (const material of materials) {
        if (!(material instanceof THREE.MeshStandardMaterial)) continue
        const modelTint = material.userData.s2ModelTint as
          | THREE.Color
          | undefined
        modelTint?.copy(tint)
      }
    })

    // Find the bounding sphere of what is drawn.
    group.updateMatrixWorld(true)
    const box = new THREE.Box3().setFromObject(group)
    if (box.isEmpty()) throw new Error('The model draws nothing')
    const center = box.getCenter(new THREE.Vector3())
    const radius = box.getSize(new THREE.Vector3()).length() / 2

    // Place the camera at yaw and pitch in the model's Z up frame, which
    // the stage turns Y up, far enough back that the sphere fits.
    const yaw = THREE.MathUtils.degToRad(shot.yaw ?? 0)
    const pitch = THREE.MathUtils.degToRad(shot.pitch ?? 0)
    const fov = shot.fov || 30
    const toward = new THREE.Vector3(
      Math.cos(pitch) * Math.cos(yaw),
      Math.sin(pitch),
      -Math.cos(pitch) * Math.sin(yaw),
    )
    const distance =
      (radius / Math.sin(THREE.MathUtils.degToRad(fov) / 2)) * 1.02

    // Aim the camera with its clip planes hugging the sphere.
    this.camera.fov = fov
    this.camera.aspect = 1
    this.camera.near = Math.max(distance - radius * 1.1, distance / 1000)
    this.camera.far = distance + radius * 1.1
    this.camera.position.copy(center).addScaledVector(toward, distance)
    this.camera.lookAt(center)
    this.camera.updateProjectionMatrix()
    this.camera.updateMatrixWorld()

    // Light from the camera's upper left front, rimmed from its upper right
    // behind.
    const along = (x: number, y: number, z: number) =>
      new THREE.Vector3(x, y, z)
        .transformDirection(this.camera.matrixWorld)
        .multiplyScalar(distance)
        .add(center)
    this.key.position.copy(along(-0.6, 0.8, 0.6))
    this.rim.position.copy(along(0.7, 0.5, -0.9))
    this.key.target.position.copy(center)
    this.rim.target.position.copy(center)

    // Draw at twice the size, then again larger when the model fills less
    // of the square than the picture needs.
    const size = shot.size || 1024
    const margin = Math.min(Math.max(shot.margin ?? 0.04, 0), 0.4)
    const span = size * (1 - 2 * margin)
    const limit = Math.min(largest, this.renderer.capabilities.maxTextureSize)

    // Draw, then redraw larger when the first draw was too small.
    let side = Math.min(limit, size * supersample)
    let drawn = this.draw(side)
    const fill = Math.max(drawn.crop.width, drawn.crop.height) / side
    const needed = Math.min(limit, Math.ceil((span * supersample) / fill))
    if (needed > side) {
      side = needed
      drawn = this.draw(side)
    }
    signal.throwIfAborted()

    // Scale the crop so its longer side spans the picture inside the margin.
    const { pixels, crop } = drawn
    const scale = span / Math.max(crop.width, crop.height)
    const pad = Math.round(size * margin)

    // Copy the crop into a picture of that size with the margin around it.
    const out = document.createElement('canvas')
    out.width = Math.round(crop.width * scale) + 2 * pad
    out.height = Math.round(crop.height * scale) + 2 * pad
    const context = out.getContext('2d')!
    context.imageSmoothingEnabled = true
    context.imageSmoothingQuality = 'high'
    context.drawImage(
      pixels,
      crop.x,
      crop.y,
      crop.width,
      crop.height,
      pad,
      pad,
      out.width - 2 * pad,
      out.height - 2 * pad,
    )

    // Encode it as a PNG.
    const png = await new Promise<Blob>((resolve, reject) =>
      out.toBlob(
        (blob) => (blob ? resolve(blob) : reject(new Error('No PNG'))),
        'image/png',
      ),
    )
    return ShotResult.create({
      png: new Uint8Array(await png.arrayBuffer()),
      missing,
    })
  }

  // draw renders the stage into a square of side pixels and returns it on a
  // 2D canvas with the box around every pixel it covers.
  private draw(side: number) {
    // Render, and copy the frame while the drawing buffer holds it.
    this.renderer.setSize(side, side, false)
    this.renderer.render(this.scene, this.camera)
    const pixels = document.createElement('canvas')
    pixels.width = pixels.height = side
    const context = pixels.getContext('2d', { willReadFrequently: true })!
    context.drawImage(this.canvas, 0, 0)

    // Read the frame back.
    const { data } = context.getImageData(0, 0, side, side)

    // Scan for the covered pixels' bounds.
    let left = side
    let top = side
    let right = -1
    let bottom = -1
    for (let y = 0; y < side; y++) {
      const row = y * side * 4
      for (let x = 0; x < side; x++) {
        if (data[row + x * 4 + 3] === 0) continue
        if (x < left) left = x
        if (x > right) right = x
        if (y < top) top = y
        bottom = y
      }
    }

    // Fail when the model covers nothing; otherwise box what it covers.
    if (right < 0) throw new Error('The model covers no pixel')
    const crop = {
      x: left,
      y: top,
      width: right - left + 1,
      height: bottom - top + 1,
    }
    return { pixels, crop }
  }

  // load replaces the stage's model with the default level of detail and
  // mesh groups of name, once its textures have loaded or failed.
  private async load(name: string, signal: AbortSignal): Promise<Loaded> {
    // Free the last model.
    if (this.loaded) {
      this.scene.remove(this.loaded.group)
      this.loaded.group.traverse((object) => {
        if (object instanceof THREE.Mesh)
          (object as THREE.Mesh).geometry.dispose()
      })
      this.loaded.materials.dispose()
      this.loaded = null
    }

    // Read the model and keep the calls the game draws by default.
    const model = await fetchMessage(Model, route('model', name), signal)
    const groups = defaultGroups(model)
    const missing: string[] = []
    const materials = new Materials(
      new TextureCache(this.renderer, largest, signal, (texture, reason) =>
        missing.push(`${texture}: ${reason}`),
      ),
    )
    const parts = new Parts(name, model, signal)
    const shown = calls(model).filter((call) => {
      const mesh = model.meshes![call.mesh]!
      return (
        ((mesh.lodMask ?? 1n) & 1n) !== 0n &&
        (groups === null || ((mesh.groupMask ?? 0n) & groups) !== 0n)
      )
    })
    if (!shown.length) throw new Error('The model has no meshes to draw')

    // Build each call in its material; Source 2 is Z up and three.js Y up.
    const group = new THREE.Group()
    group.rotation.x = -Math.PI / 2
    for (const call of shown) {
      const geometry = await parts.geometry(call)
      if (!geometry) continue
      const material = call.call.material ?? ''
      group.add(
        new THREE.Mesh(
          geometry,
          materials.get(
            material,
            model.materials?.[material],
            geometry.hasAttribute('color'),
          ),
        ),
      )
    }

    // Wait for every texture, then compile so the first draw is complete.
    await materials.textures.settled()
    signal.throwIfAborted()
    this.scene.add(group)
    await this.renderer.compileAsync(group, this.camera, this.scene)
    return { name, group, materials, missing }
  }
}

// runRender draws the server's render job, reporting progress in status,
// and sends each shot's answer back as it finishes.
export async function runRender(status: HTMLElement) {
  // Read the job.
  const job = await fetchMessage(RenderJob, '/api/render/job')
  const shots = job.shots ?? []
  const stage = new Stage()

  // Draw each shot, failing one that errs or runs too long.
  for (const [index, shot] of shots.entries()) {
    status.textContent = `Rendering ${index + 1} of ${shots.length}: ${shot.model}`
    const signal = AbortSignal.timeout(shotTimeout)
    const result = await stage
      .shoot(shot, signal)
      .catch((error: Error) => ShotResult.create({ error: error.message }))
    await fetch(`/api/render/shot/${index}`, {
      method: 'POST',
      body: new Uint8Array(ShotResult.toBinary(result)),
    })
  }
  status.textContent = `Rendered ${shots.length} shots`
}
