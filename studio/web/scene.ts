// Draws one game asset with three.js: a model with the game's materials, a
// whole map, or a texture on a plane. Every byte comes from the local
// server, which reads it from the player's own install; vertex and index
// buffers arrive meshopt-encoded and textures in their stored GPU block
// formats, so the browser decodes buffers and the GPU decodes texels.
import * as THREE from 'three'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import { RoomEnvironment } from 'three/examples/jsm/environments/RoomEnvironment.js'
import type { Model, Texture, World } from '../studio.pb.js'
import { calls, Materials, Parts } from './models.js'
import { loadTexture, TextureCache } from './textures.js'
import { buildWorld, type WorldStats } from './world.js'

export { formatName, Unavailable } from './textures.js'
export type { WorldStats } from './world.js'

// Shading is how the viewport draws a model's surfaces.
export type Shading = 'material' | 'solid' | 'wireframe'

// Stats counts what the viewport draws.
export interface Stats {
  drawCalls: number
  vertices: number
  triangles: number
}

// Selection picks which of a model's meshes draw: one level of detail and
// the mesh groups in a mask, or every group when the model names none.
export interface Selection {
  lod: number
  groups: bigint | null
}

// LoadedModel is the model the viewport holds and the draw calls it has
// shown so far, by index across its meshes.
interface LoadedModel {
  parts: Parts
  signal: AbortSignal
  shown: Map<number, Promise<THREE.Mesh | null>>
  framed: boolean
}

// Model textures load up to this size; a map's many textures load smaller.
const modelTextureLimit = 2048
const worldTextureLimit = 512

// The solid and wireframe shadings draw every surface in one material.
const solid = new THREE.MeshStandardMaterial({
  color: 0xc9c1b4,
  roughness: 0.8,
})
const wire = new THREE.MeshStandardMaterial({
  color: 0xc9c1b4,
  roughness: 0.8,
  wireframe: true,
})

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
  private materials: Materials | null = null
  private resize: ResizeObserver

  // onMissing hears each texture of the drawn asset that cannot be drawn.
  constructor(
    private canvas: HTMLCanvasElement,
    private onMissing: (texture: string, reason: string) => void,
  ) {
    // Draw at the screen's pixel density over the page's own background,
    // tone mapped as a game frame is.
    this.renderer = new THREE.WebGLRenderer({
      canvas,
      antialias: true,
      alpha: true,
    })
    this.renderer.setPixelRatio(window.devicePixelRatio)
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping

    // Light the scene from a soft studio room for reflections and ambient
    // light, and one sun for shape.
    const pmrem = new THREE.PMREMGenerator(this.renderer)
    this.scene.environment = pmrem.fromScene(
      new RoomEnvironment(),
      0.04,
    ).texture
    this.scene.environmentIntensity = 0.55
    pmrem.dispose()
    const sun = new THREE.DirectionalLight(0xfff4e6, 1.8)
    sun.position.set(1.2, 1.4, 0.8)
    this.scene.add(sun, this.content)

    // Orbit with the pointer, follow the canvas's size and draw every frame.
    this.controls = new OrbitControls(this.camera, canvas)
    this.controls.enableDamping = true
    this.controls.zoomToCursor = true
    this.resize = new ResizeObserver(() => this.fit())
    this.resize.observe(canvas.parentElement!)
    this.renderer.setAnimationLoop(() => {
      this.controls.update()
      this.renderer.render(this.scene, this.camera)
    })
  }

  // dispose releases the renderer.
  dispose() {
    // Stop drawing, then free the asset and the renderer.
    this.resize.disconnect()
    this.renderer.setAnimationLoop(null)
    this.clear()
    this.scene.environment?.dispose()
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
    // Free every mesh's geometry and own material, then the asset's shared
    // materials and textures.
    this.content.traverse((object) => {
      // Free this mesh's geometry, instances and own material.
      if (!(object instanceof THREE.Mesh)) return
      const mesh = object as THREE.Mesh
      mesh.geometry.dispose()
      if (mesh instanceof THREE.InstancedMesh) mesh.dispose()
      const own = mesh.userData.own as THREE.MeshBasicMaterial | undefined
      own?.map?.dispose()
      own?.dispose()
    })
    this.materials?.dispose()
    this.materials = null
    this.content.clear()
    this.content.rotation.set(0, 0, 0)
    this.loaded = null
    this.setGridSize(null)
  }

  // setShading draws every surface in the given shading.
  setShading(shading: Shading) {
    this.shading = shading
    this.content.traverse((object) => {
      if (object instanceof THREE.Mesh && object.userData.material)
        this.shade(object as THREE.Mesh)
    })
  }

  // shade applies the shading to one of a model's meshes, which keeps its
  // own material in userData.
  private shade(mesh: THREE.Mesh) {
    if (this.shading === 'solid') mesh.material = solid
    else if (this.shading === 'wireframe') mesh.material = wire
    else mesh.material = mesh.userData.material as THREE.Material
  }

  // add adds a mesh drawn in its material and the current shading.
  private add(mesh: THREE.Mesh) {
    mesh.userData.material = mesh.material
    this.shade(mesh)
    this.content.add(mesh)
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
    // Find the visible meshes' bounds.
    const box = new THREE.Box3()
    this.content.updateMatrixWorld()
    this.content.traverseVisible((object) => {
      if (object instanceof THREE.Mesh) box.expandByObject(object)
    })
    if (box.isEmpty()) return
    const size = box.getSize(new THREE.Vector3()).length() || 1
    const center = box.getCenter(new THREE.Vector3())

    // Back off until the bounding sphere fits the narrower of the two fields
    // of view, with a margin.
    const vertical = THREE.MathUtils.degToRad(this.camera.fov) / 2
    const horizontal = Math.atan(Math.tan(vertical) * this.camera.aspect)
    const distance =
      (size / 2 / Math.sin(Math.min(vertical, horizontal))) * 1.05
    this.controls.target.copy(center)
    this.camera.position
      .copy(center)
      .add(direction.normalize().multiplyScalar(distance))
    this.camera.near = size / 5000
    this.camera.far = size * 100
    this.camera.updateProjectionMatrix()
  }

  // replace clears what is drawn and starts an asset whose textures load up
  // to limit. Source 2 is Z up and three.js Y up, so content turns.
  private replace(limit: number, signal: AbortSignal) {
    this.clear()
    this.materials = new Materials(
      new TextureCache(this.renderer, limit, signal, this.onMissing),
    )
    this.content.rotation.x = -Math.PI / 2
    return this.materials
  }

  // loadModel replaces what is drawn with a model, drawing nothing until
  // showModel picks its meshes. signal ends every load the model starts.
  loadModel(name: string, model: Model, signal: AbortSignal) {
    this.replace(modelTextureLimit, signal)
    this.loaded = {
      parts: new Parts(name, model, signal),
      signal,
      shown: new Map(),
      framed: false,
    }
  }

  // showModel draws the loaded model's meshes the selection picks, building
  // each draw call the first time it shows. It frames the model the first
  // time. Textures arrive after the draw calls.
  async showModel(selection: Selection): Promise<Stats> {
    // Build every draw call the selection shows.
    const loaded = this.loaded
    if (!loaded) throw new Error('No model is loaded')
    const model = loaded.parts.model
    const shown: Promise<THREE.Mesh | null>[] = []
    const hidden: Promise<THREE.Mesh | null>[] = []
    for (const call of calls(model)) {
      const mesh = model.meshes![call.mesh]!
      const visible =
        (((mesh.lodMask ?? 1n) >> BigInt(selection.lod)) & 1n) !== 0n &&
        (selection.groups === null ||
          ((mesh.groupMask ?? 0n) & selection.groups) !== 0n)
      let built = loaded.shown.get(call.flat)
      if (!built && visible) {
        built = this.buildCall(loaded, call)
        loaded.shown.set(call.flat, built)
      }
      if (built) (visible ? shown : hidden).push(built)
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
      this.frame()
      this.setGridSize(new THREE.Box3().setFromObject(this.content))
    }
    return stats
  }

  // buildCall builds one draw call's mesh and adds it hidden, or returns
  // null for a call whose buffers are missing.
  private async buildCall(
    loaded: LoadedModel,
    call: ReturnType<typeof calls>[number],
  ): Promise<THREE.Mesh | null> {
    // Build the geometry and draw it in the call's material.
    const geometry = await loaded.parts.geometry(call)
    loaded.signal.throwIfAborted()
    if (!geometry || this.loaded !== loaded) return null
    const name = call.call.material ?? ''
    const drawn = new THREE.Mesh(
      geometry,
      this.materials!.get(
        name,
        loaded.parts.model.materials?.[name],
        geometry.hasAttribute('color'),
      ),
    )

    // Add it hidden; showModel decides what shows.
    const mesh = loaded.parts.model.meshes![call.mesh]!
    drawn.name = `${mesh.name ?? ''}: ${name}`
    drawn.userData.vertexBuffer = `${call.mesh}/${call.call.vertexBuffers?.[0] ?? 0}`
    drawn.visible = false
    this.add(drawn)
    return drawn
  }

  // loadWorld replaces what is drawn with a map, adding each model as it
  // loads and reporting how many have. It frames the map from above once
  // the first models show and again when every model has.
  async loadWorld(
    world: World,
    signal: AbortSignal,
    onProgress: (loaded: number) => void,
  ): Promise<WorldStats> {
    // Draw each batch as it is built.
    const materials = this.replace(worldTextureLimit, signal)
    const above = new THREE.Vector3(0.4, 1, 0.6)
    let loaded = 0
    const stats = await buildWorld(
      world,
      materials,
      signal,
      (mesh) => {
        if (this.materials === materials) this.add(mesh)
      },
      () => {
        onProgress(++loaded)
        if (loaded === 8) this.frame(above)
      },
    )
    if (this.materials !== materials)
      throw new DOMException('replaced', 'AbortError')
    this.frame(above)
    return stats
  }

  // showTexture draws a texture head on, on a plane at its aspect ratio, and
  // returns the size of the mip it shows.
  async showTexture(
    name: string,
    header: Texture,
    signal: AbortSignal,
  ): Promise<{ width: number; height: number }> {
    // Read the mips from the largest that fits 2048, unchanged by lighting
    // or tone mapping.
    const format = header.format ?? 0
    const { texture, width, height } = await loadTexture(
      this.renderer,
      name,
      header,
      2048,
      format !== 21 && format !== 27,
      signal,
    )
    signal.throwIfAborted()
    const material = new THREE.MeshBasicMaterial({
      map: texture,
      side: THREE.DoubleSide,
      toneMapped: false,
    })

    // Size a plane to the texture's aspect.
    const w = header.width ?? 1
    const h = header.height ?? 1
    const longest = Math.max(w, h)
    const plane = new THREE.Mesh(
      new THREE.PlaneGeometry(w / longest, h / longest),
      material,
    )
    plane.userData.own = material

    // Replace what was drawn.
    this.clear()
    this.content.add(plane)
    this.frame(new THREE.Vector3(0, 0, 1))
    return { width, height }
  }
}
