// The tools page: a searchable list of every file in the game's archive on
// the left and the selected asset drawn on the right.
import { useEffect, useMemo, useReducer, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { fetchJSON, Viewer, type Shown } from './scene'

// Kind filters the list by extension.
const kinds = [
  { label: 'Models', ext: '.vmdl_c' },
  { label: 'Textures', ext: '.vtex_c' },
  { label: 'Materials', ext: '.vmat_c' },
  { label: 'All', ext: '' },
] as const

// how says how the page shows a file.
const how = (name: string) =>
  name.endsWith('.vmdl_c')
    ? 'model'
    : name.endsWith('.vtex_c')
      ? 'texture'
      : 'kv3'

// View is the right side's state machine.
type View =
  | { state: 'idle' }
  | { state: 'loading'; name: string }
  | { state: 'drawn'; name: string; shown: Shown }
  | { state: 'data'; name: string; text: string }
  | { state: 'failed'; name: string; message: string }

// ViewEvent moves the view: a selection, or the result of loading one.
type ViewEvent =
  | { type: 'select'; name: string }
  | { type: 'drawn'; name: string; shown: Shown }
  | { type: 'data'; name: string; text: string }
  | { type: 'failed'; name: string; message: string }

// reduceView applies an event; a result for a file no longer selected is
// dropped.
function reduceView(view: View, event: ViewEvent): View {
  if (event.type === 'select') return { state: 'loading', name: event.name }
  if (view.state !== 'loading' || view.name !== event.name) return view
  switch (event.type) {
    case 'drawn':
      return { state: 'drawn', name: event.name, shown: event.shown }
    case 'data':
      return { state: 'data', name: event.name, text: event.text }
    case 'failed':
      return { state: 'failed', name: event.name, message: event.message }
  }
}

// Listing is the archive's file list as it loads.
type Listing =
  | { state: 'loading' }
  | { state: 'ready'; names: string[] }
  | { state: 'failed'; message: string }

// rowHeight is one list row's height in pixels, as app.css sets it.
const rowHeight = 24

// Rows is a virtual list: it renders only the rows in view.
function Rows(props: {
  names: string[]
  selected: string
  onSelect: (name: string) => void
}) {
  // The list box and how far it is scrolled.
  const box = useRef<HTMLDivElement>(null)
  const [scroll, setScroll] = useState({ top: 0, height: 800 })

  // Track the list box's height as the window resizes.
  useEffect(() => {
    // Measure the box now and on every resize.
    const element = box.current!
    const measure = () =>
      setScroll({ top: element.scrollTop, height: element.clientHeight })
    measure()
    const resize = new ResizeObserver(measure)
    resize.observe(element)
    return () => resize.disconnect()
  }, [])

  // Render the rows between the top and bottom of the box.
  const first = Math.floor(scroll.top / rowHeight)
  const last = Math.min(
    props.names.length,
    first + Math.ceil(scroll.height / rowHeight) + 1,
  )
  const rows = props.names.slice(first, last).map((name, i) => (
    <div
      key={name}
      className="row"
      role="option"
      aria-selected={name === props.selected}
      style={{ top: (first + i) * rowHeight }}
      title={name}
      onClick={() => props.onSelect(name)}
    >
      <bdi>{name}</bdi>
    </div>
  ))
  return (
    <div
      ref={box}
      className="rows"
      role="listbox"
      onScroll={(e) =>
        setScroll({
          top: e.currentTarget.scrollTop,
          height: e.currentTarget.clientHeight,
        })
      }
    >
      <div style={{ height: props.names.length * rowHeight }}>{rows}</div>
    </div>
  )
}

function App() {
  // The file list, the filters and the view of the selected file.
  const [listing, setListing] = useState<Listing>({ state: 'loading' })
  const [query, setQuery] = useState('')
  const [kind, setKind] = useState<string>('.vmdl_c')
  const [view, dispatch] = useReducer(reduceView, { state: 'idle' })
  const canvas = useRef<HTMLCanvasElement>(null)
  const viewer = useRef<Viewer | null>(null)

  // Read the archive's file list once.
  useEffect(() => {
    fetchJSON<{ names: string[] }>('/api/assets/list')
      .then((body) => setListing({ state: 'ready', names: body.names.sort() }))
      .catch((error: Error) =>
        setListing({ state: 'failed', message: error.message }),
      )
  }, [])

  // The hash holds the selected file, so a link reopens it.
  useEffect(() => {
    const follow = () => {
      const name = decodeURIComponent(location.hash.slice(1))
      if (name) dispatch({ type: 'select', name })
    }
    follow()
    window.addEventListener('hashchange', follow)
    return () => window.removeEventListener('hashchange', follow)
  }, [])

  // One viewer draws every model and texture for the page's lifetime.
  useEffect(() => {
    viewer.current = new Viewer(canvas.current!)
    return () => viewer.current?.dispose()
  }, [])

  // Load the selected file. Its textures may still arrive after it is
  // drawn, so only the next selection aborts it.
  const current = useRef<AbortController | null>(null)
  const loading = view.state === 'loading' ? view.name : ''
  useEffect(() => {
    // Replace any earlier load.
    if (!loading) return
    current.current?.abort()
    const abort = new AbortController()
    current.current = abort
    const fail = (error: Error) =>
      !abort.signal.aborted &&
      dispatch({ type: 'failed', name: loading, message: error.message })

    // Draw models and textures; show any other file's KeyValues3 blocks.
    const kindOf = how(loading)
    if (kindOf === 'kv3') {
      fetchJSON<unknown>(`/api/assets/kv3/${loading}`, abort.signal)
        .then((body) =>
          dispatch({
            type: 'data',
            name: loading,
            text: JSON.stringify(body, null, 2),
          }),
        )
        .catch(fail)
      return
    }
    const shown =
      kindOf === 'model'
        ? viewer.current!.showModel(loading, abort.signal)
        : viewer.current!.showTexture(loading, abort.signal)
    shown
      .then((shown) => dispatch({ type: 'drawn', name: loading, shown }))
      .catch(fail)
  }, [loading])

  // Filter the list by kind and by every word of the search.
  const names = useMemo(() => {
    if (listing.state !== 'ready') return []
    const words = query.toLowerCase().split(/\s+/).filter(Boolean)
    return listing.names.filter(
      (name) =>
        name.endsWith(kind) && words.every((word) => name.includes(word)),
    )
  }, [listing, query, kind])

  // The canvas stays mounted and shows while a model or texture is up.
  const selected = view.state === 'idle' ? '' : view.name
  const canvasShown =
    view.state === 'drawn' ||
    (view.state === 'loading' && how(view.name) !== 'kv3')

  return (
    <div className="app">
      <aside className="side">
        <header>
          <h1>Modlock tools</h1>
          <input
            type="search"
            placeholder="Search files"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            aria-label="Search files"
          />
          <div className="kinds">
            {kinds.map((k) => (
              <button
                key={k.label}
                aria-pressed={kind === k.ext}
                onClick={() => setKind(k.ext)}
              >
                {k.label}
              </button>
            ))}
          </div>
          <span className="count">
            {listing.state === 'loading' && 'Reading the game archive…'}
            {listing.state === 'failed' &&
              `Could not read the game archive: ${listing.message}`}
            {listing.state === 'ready' &&
              `${names.length.toLocaleString()} files`}
          </span>
        </header>
        <Rows
          names={names}
          selected={selected}
          onSelect={(name) => (location.hash = encodeURIComponent(name))}
        />
      </aside>
      <main className="view">
        <canvas ref={canvas} hidden={!canvasShown} />
        {view.state === 'idle' && <div className="status">Choose a file</div>}
        {view.state === 'loading' && (
          <div className="status">Loading {view.name}</div>
        )}
        {view.state === 'failed' && (
          <div className="status">{view.message}</div>
        )}
        {view.state === 'data' && <pre className="kv3">{view.text}</pre>}
        {view.state === 'drawn' && (
          <div className="info">
            <div>{view.name}</div>
            <div>{view.shown.detail}</div>
          </div>
        )}
      </main>
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<App />)
