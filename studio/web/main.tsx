// Modlock Studio's page: the archive's outliner on the left, the selected
// asset in the viewport, its properties on the right, and a status bar.
import { useEffect, useReducer, useRef, type Dispatch } from 'react'
import { createRoot } from 'react-dom/client'
import { Listing, Model, Resource, Texture, World } from '../studio.pb.js'
import { fetchMessage, kindOf, route } from './api.js'
import { Icon } from './icons.js'
import { collisionCounts, hasCollision, hasMeshes } from './models.js'
import { Outliner } from './outliner.js'
import { Properties } from './properties.js'
import { formatName, Unavailable, Viewer, type Shading } from './scene.js'
import {
  initial,
  reduce,
  type Asset,
  type Event,
  type Studio,
} from './state.js'
import { buildTree } from './tree.js'

// select puts a file in the address's hash, which selects it, so a link or
// a reload reopens it.
const select = (name: string) => {
  location.hash = encodeURIComponent(name)
}

// load reads the selected file and draws it, reporting each step. Only the
// next selection aborts it, through signal.
function load(
  name: string,
  viewer: Viewer,
  dispatch: Dispatch<Event>,
  signal: AbortSignal,
) {
  // Report a failure unless the load was replaced.
  const fail = (error: Error) => {
    if (signal.aborted) return
    if (error instanceof Unavailable)
      dispatch({ type: 'unavailable', name, reason: error.message })
    else dispatch({ type: 'failed', name, message: error.message })
  }

  // Maps, models and textures draw in the viewport; anything else shows its
  // data.
  switch (kindOf(name)) {
    case 'map':
      void fetchMessage(World, route('world', name), signal)
        .then(async (world) => {
          dispatch({ type: 'map', name, world })
          const stats = await viewer.loadWorld(world, signal, (loaded) =>
            dispatch({ type: 'progress', name, loaded }),
          )
          dispatch({ type: 'built', name, stats })
        })
        .catch(fail)
      return
    case 'model':
      void fetchMessage(Model, route('model', name), signal)
        .then((model) => {
          viewer.loadModel(name, model, signal)
          dispatch({ type: 'model', name, model })
        })
        .catch(fail)
      return
    case 'texture':
      void fetchMessage(Texture, route('texture', name), signal)
        .then(async (texture) => {
          dispatch({ type: 'texture', name, texture })
          const shown = await viewer.showTexture(name, texture, signal)
          dispatch({ type: 'shown', name, ...shown })
        })
        .catch(fail)
      return
    default:
      void fetchMessage(Resource, route('resource', name), signal)
        .then((resource) => {
          viewer.clear()
          dispatch({ type: 'data', name, resource })
        })
        .catch(fail)
  }
}

// drawsInViewport reports whether the viewport's canvas shows for an asset.
function drawsInViewport(asset: Asset): boolean {
  if (asset.state === 'model' || asset.state === 'map') return true
  if (asset.state === 'texture') return asset.unavailable === null
  return asset.state === 'loading' && kindOf(asset.name) !== 'data'
}

// App is the whole page, driven by one reducer.
function App() {
  // Hold the page's state and the viewer that draws into the canvas.
  const [studio, dispatch] = useReducer(reduce, initial)
  const canvas = useRef<HTMLCanvasElement>(null)
  const viewer = useRef<Viewer | null>(null)
  const { asset } = studio

  // Read the archive's file list once.
  useEffect(() => {
    fetchMessage(Listing, '/api/assets/list')
      .then((listing) => dispatch({ type: 'listed', tree: buildTree(listing) }))
      .catch((error: Error) =>
        dispatch({ type: 'unlisted', message: error.message }),
      )
  }, [])

  // The hash holds the selected file.
  useEffect(() => {
    const follow = () => {
      const name = decodeURIComponent(location.hash.slice(1))
      if (name) dispatch({ type: 'select', name })
    }
    follow()
    window.addEventListener('hashchange', follow)
    return () => window.removeEventListener('hashchange', follow)
  }, [])

  // One viewer draws every model and texture for the page's lifetime. A
  // missing texture belongs to the model selected when it is found.
  const name = asset.state === 'idle' ? '' : asset.name
  const selected = useRef(name)
  selected.current = name
  useEffect(() => {
    viewer.current = new Viewer(canvas.current!, (texture, reason) =>
      dispatch({ type: 'missing', name: selected.current, texture, reason }),
    )
    return () => viewer.current?.dispose()
  }, [])

  // Load each selected file, aborting the one before.
  const loading = asset.state === 'loading' ? asset.name : ''
  const current = useRef<AbortController | null>(null)
  useEffect(() => {
    // Replace the load in flight with this one.
    if (!loading) return
    current.current?.abort()
    const abort = new AbortController()
    current.current = abort
    load(loading, viewer.current!, dispatch, abort.signal)
  }, [loading])

  // When the GPU resets, drop the load in flight and draw the file again
  // once the context is back, since the GPU kept none of it.
  useEffect(() => {
    // Listen for the canvas losing and regaining its context.
    const target = canvas.current!
    const lost = () => {
      current.current?.abort()
      viewer.current?.clear()
      dispatch({ type: 'lost' })
    }
    const restored = () => dispatch({ type: 'restored' })
    target.addEventListener('webglcontextlost', lost)
    target.addEventListener('webglcontextrestored', restored)
    return () => {
      target.removeEventListener('webglcontextlost', lost)
      target.removeEventListener('webglcontextrestored', restored)
    }
  }, [])

  // Draw the model's chosen level of detail and mesh groups.
  const lod = asset.state === 'model' ? asset.lod : -1
  const groups = asset.state === 'model' ? asset.groups : null
  useEffect(() => {
    if (lod < 0) return
    const signal = current.current!.signal
    viewer
      .current!.showModel({ lod, groups })
      .then((stats) => dispatch({ type: 'drawn', name, stats }))
      .catch((error: Error) => {
        if (!signal.aborted && error.name !== 'AbortError')
          dispatch({ type: 'failed', name, message: error.message })
      })
  }, [name, lod, groups])

  // Apply the viewport's display options.
  useEffect(() => viewer.current?.setShading(studio.shading), [studio.shading])
  useEffect(() => viewer.current?.setGrid(studio.grid), [studio.grid])
  useEffect(
    () => viewer.current?.setCollision(studio.collision),
    [studio.collision],
  )

  // F frames the asset, as in other 3D tools, unless a field has focus.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const typing = (e.target as HTMLElement).closest('input, textarea')
      if (e.key.toLowerCase() === 'f' && !typing && !e.ctrlKey && !e.metaKey)
        viewer.current?.frame()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  return (
    <div className="studio">
      <TopBar name={name} dispatch={dispatch} />
      <Outliner
        archive={studio.archive}
        outliner={studio.outliner}
        selected={name}
        dispatch={dispatch}
        onSelect={select}
      />
      <main className="viewport" aria-label="Viewport">
        <ViewportHeader
          studio={studio}
          dispatch={dispatch}
          onFrame={() => viewer.current?.frame()}
        />
        <div className="stage">
          <canvas ref={canvas} hidden={!drawsInViewport(asset)} />
          <StageNote asset={asset} />
          {asset.state === 'data' && (
            <pre className="data">{asset.resource.json}</pre>
          )}
        </div>
      </main>
      <Properties asset={asset} dispatch={dispatch} onSelect={select} />
      <StatusBar studio={studio} />
    </div>
  )
}

// TopBar shows the studio's mark and the selected file's path, whose
// folders reveal themselves in the outliner.
function TopBar(props: { name: string; dispatch: Dispatch<Event> }) {
  const parts = props.name ? props.name.split('/') : []
  return (
    <header className="topbar">
      <h1 className="mark">
        <span className="glyph" aria-hidden="true" />
        Modlock Studio
      </h1>
      <nav className="crumbs" aria-label="Path">
        {parts.map((part, i) => {
          // Each folder reveals itself; the file is the current page.
          const path = parts.slice(0, i + 1).join('/')
          const last = i === parts.length - 1
          return (
            <span key={path} className="crumb">
              {i > 0 && <span className="sep">/</span>}
              {last ? (
                <span aria-current="page">{part}</span>
              ) : (
                <button
                  onClick={() => props.dispatch({ type: 'reveal', path })}
                >
                  {part}
                </button>
              )}
            </span>
          )
        })}
      </nav>
    </header>
  )
}

// shadings are the viewport's shading choices.
const shadings: {
  shading: Shading
  label: string
  icon: 'solid' | 'texture' | 'wireframe'
}[] = [
  { shading: 'wireframe', label: 'Wireframe', icon: 'wireframe' },
  { shading: 'solid', label: 'Solid', icon: 'solid' },
  { shading: 'material', label: 'Material', icon: 'texture' },
]

// ViewportHeader names what the viewport shows and holds its display
// options, which apply only to a drawn model.
function ViewportHeader(props: {
  studio: Studio
  dispatch: Dispatch<Event>
  onFrame: () => void
}) {
  // Shading shows for a model or a map, the grid for a model, and Frame for
  // anything drawn.
  const { studio, dispatch } = props
  const { asset } = studio
  const model = asset.state === 'model'
  const shaded = model || asset.state === 'map'
  const overlay = model && hasMeshes(asset.model) && hasCollision(asset.model)
  const drawn = drawsInViewport(asset)
  return (
    <header className="viewport-head">
      <span className="title">
        {asset.state !== 'idle' && <Icon name={kindOf(asset.name)} />}
        {asset.state === 'idle'
          ? 'Viewport'
          : asset.name.slice(asset.name.lastIndexOf('/') + 1)}
      </span>
      {shaded && (
        <div className="segmented" role="group" aria-label="Shading">
          {shadings.map((s) => (
            <button
              key={s.shading}
              aria-pressed={studio.shading === s.shading}
              title={`${s.label} shading`}
              onClick={() => dispatch({ type: 'shading', shading: s.shading })}
            >
              <Icon name={s.icon} />
              {s.label}
            </button>
          ))}
        </div>
      )}
      {model && (
        <button
          className="tool"
          aria-pressed={studio.grid}
          title="Show the floor grid"
          onClick={() => dispatch({ type: 'grid' })}
        >
          <Icon name="grid" />
          Grid
        </button>
      )}
      {overlay && (
        <button
          className="tool"
          aria-pressed={studio.collision}
          title="Show the collision shapes"
          onClick={() => dispatch({ type: 'collision' })}
        >
          <Icon name="collision" />
          Collision
        </button>
      )}
      {drawn && (
        <button
          className="tool"
          title="Frame the asset (F)"
          onClick={props.onFrame}
        >
          <Icon name="frame" />
          Frame
        </button>
      )}
    </header>
  )
}

// StageNote says what the viewport waits for, or why it shows nothing.
function StageNote(props: { asset: Asset }) {
  // Only the idle, failed and unavailable states leave the stage empty.
  const { asset } = props
  let note = ''
  if (asset.state === 'idle')
    note = 'Choose a map, model, texture or material in the outliner'
  if (asset.state === 'failed') note = asset.message
  if (asset.state === 'lost')
    note = 'The graphics card reset. Drawing resumes when it is back.'
  if (asset.state === 'texture' && asset.unavailable) note = asset.unavailable

  // A model with nothing to draw says so.
  if (
    asset.state === 'model' &&
    !hasMeshes(asset.model) &&
    !hasCollision(asset.model)
  )
    note = 'This model has no meshes or collision shapes'
  if (!note) return null
  return <p className="stage-note">{note}</p>
}

// StatusBar shows the archive's state and what the viewport draws.
function StatusBar(props: { studio: Studio }) {
  // The left side says how much of the archive is listed.
  const { archive, asset } = props.studio
  let left = ''
  if (archive.state === 'loading') left = 'Reading the game archive…'
  if (archive.state === 'gone') left = 'No game archive'
  if (archive.state === 'ready')
    left = `${archive.tree.paths.length.toLocaleString()} files in the game archive`

  // The right side counts what the viewport draws.
  let right = ''
  if (asset.state === 'loading') right = `Loading ${asset.name}`
  if (asset.state === 'model' && !hasMeshes(asset.model)) {
    const { shapes, triangles } = collisionCounts(asset.model)
    right = `${shapes.toLocaleString()} collision shapes · ${triangles.toLocaleString()} triangles`
  } else if (asset.state === 'model' && asset.stats) {
    const { drawCalls, vertices, triangles } = asset.stats
    right = `${drawCalls} draw calls · ${vertices.toLocaleString()} vertices · ${Math.round(triangles).toLocaleString()} triangles`
    if (asset.missing.length)
      right += ` · ${asset.missing.length} textures not drawn`
  }
  if (asset.state === 'map') {
    const models = asset.world.models?.length ?? 0
    right = asset.stats
      ? `${asset.stats.drawCalls.toLocaleString()} draw calls · ${Math.round(asset.stats.triangles).toLocaleString()} triangles`
      : `Loading models ${asset.loaded.toLocaleString()} of ${models.toLocaleString()}`
    if (asset.missing.length)
      right += ` · ${asset.missing.length} textures not drawn`
  }
  if (asset.state === 'texture') {
    const { width, height, format } = asset.texture
    right = `${width}×${height} ${formatName(format ?? 0)}`
  }
  return (
    <footer className="statusbar">
      <span>{left}</span>
      <span>{right}</span>
    </footer>
  )
}

createRoot(document.getElementById('root')!).render(<App />)
