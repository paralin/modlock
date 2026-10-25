// The outliner: the archive's folders as a tree, or the files a search
// finds, in a virtual list that renders only the rows in view.
import {
  useEffect,
  useMemo,
  useRef,
  useState,
  type Dispatch,
  type KeyboardEvent,
} from 'react'
import { kindOf } from './api.js'
import { Icon } from './icons.js'
import type { Archive, Event, Outliner as OutlinerState } from './state.js'
import {
  searchRows,
  treeRows,
  type Filter,
  type Row,
  type Tree,
} from './tree.js'

// filters are the outliner's kind choices.
const filters: { filter: Filter; label: string }[] = [
  { filter: 'map', label: 'Maps' },
  { filter: 'model', label: 'Models' },
  { filter: 'texture', label: 'Textures' },
  { filter: 'material', label: 'Materials' },
  { filter: 'all', label: 'All' },
]

// rowHeight is one row's height in pixels, as studio.css sets it.
const rowHeight = 26

// size formats a byte count.
export function size(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`
}

// Outliner shows the archive and selects files.
export function Outliner(props: {
  archive: Archive
  outliner: OutlinerState
  selected: string
  dispatch: Dispatch<Event>
  onSelect: (name: string) => void
}) {
  const { archive, outliner, dispatch } = props
  return (
    <section className="panel outliner" aria-label="Outliner">
      <header className="panel-head">
        <label className="search">
          <Icon name="search" />
          <input
            type="search"
            placeholder="Search the archive"
            value={outliner.query}
            onChange={(e) => dispatch({ type: 'query', query: e.target.value })}
            aria-label="Search the archive"
          />
        </label>
        <div className="segmented" role="group" aria-label="Kind">
          {filters.map((f) => (
            <button
              key={f.filter}
              aria-pressed={outliner.filter === f.filter}
              onClick={() => dispatch({ type: 'filter', filter: f.filter })}
            >
              {f.label}
            </button>
          ))}
        </div>
      </header>
      {archive.state === 'loading' && (
        <p className="panel-note">Reading the game archive…</p>
      )}
      {archive.state === 'gone' && (
        <p className="panel-note">
          {archive.message}. Set DEADLOCK_DIR to your Deadlock folder and run
          modlock studio again.
        </p>
      )}
      {archive.state === 'ready' && (
        <Rows
          tree={archive.tree}
          outliner={outliner}
          selected={props.selected}
          dispatch={dispatch}
          onSelect={props.onSelect}
        />
      )}
    </section>
  )
}

// Rows lists the tree or the search's results.
function Rows(props: {
  tree: Tree
  outliner: OutlinerState
  selected: string
  dispatch: Dispatch<Event>
  onSelect: (name: string) => void
}) {
  // Compute the rows for the open folders or the search.
  const { tree, outliner, dispatch } = props
  const rows = useMemo(
    () =>
      outliner.query.trim()
        ? searchRows(tree, outliner.query, outliner.filter)
        : treeRows(tree, outliner.open, outliner.filter),
    [tree, outliner.query, outliner.filter, outliner.open],
  )

  // Track the list box's scroll and height as it resizes.
  const box = useRef<HTMLDivElement>(null)
  const [scroll, setScroll] = useState({ top: 0, height: 600 })
  useEffect(() => {
    // Measure now and on every resize.
    const element = box.current!
    const measure = () =>
      setScroll({ top: element.scrollTop, height: element.clientHeight })
    measure()
    const resize = new ResizeObserver(measure)
    resize.observe(element)
    return () => resize.disconnect()
  }, [])

  // Scroll a revealed row into view.
  const pathOf = (row: Row) =>
    row.type === 'dir' ? row.dir.path : tree.paths[row.file]!
  useEffect(() => {
    // Scroll only when the row lies outside the box.
    const element = box.current
    const index = rows.findIndex((row) => pathOf(row) === outliner.reveal)
    if (!element || index < 0) return
    const top = index * rowHeight
    if (
      top < element.scrollTop ||
      top + rowHeight > element.scrollTop + element.clientHeight
    )
      element.scrollTop = top - element.clientHeight / 2
    // Only a new reveal scrolls; rows change as folders open.
    // oxlint-disable-next-line react-hooks/exhaustive-deps
  }, [outliner.reveal])

  // Move the selection with the arrow keys and open folders with Enter.
  const selectedIndex = rows.findIndex((row) => pathOf(row) === props.selected)
  const onKey = (e: KeyboardEvent) => {
    // Step to the next or previous file row.
    const step = e.key === 'ArrowDown' ? 1 : e.key === 'ArrowUp' ? -1 : 0
    if (!step) return
    e.preventDefault()
    for (let i = selectedIndex + step; i >= 0 && i < rows.length; i += step) {
      const row = rows[i]!
      if (row.type === 'file') return props.onSelect(tree.paths[row.file]!)
    }
  }

  // Render the rows between the top and bottom of the box.
  const first = Math.max(0, Math.floor(scroll.top / rowHeight) - 4)
  const last = Math.min(
    rows.length,
    first + Math.ceil(scroll.height / rowHeight) + 8,
  )
  const searching = outliner.query.trim() !== ''
  return (
    <>
      <div className="count">
        {searching
          ? `${rows.length.toLocaleString()} matches`
          : `${tree.root.counts[outliner.filter].toLocaleString()} files`}
      </div>
      <div
        ref={box}
        className="rows"
        role="tree"
        aria-label="Archive"
        tabIndex={0}
        onKeyDown={onKey}
        onScroll={(e) =>
          setScroll({
            top: e.currentTarget.scrollTop,
            height: e.currentTarget.clientHeight,
          })
        }
      >
        <div style={{ height: rows.length * rowHeight }}>
          {rows.slice(first, last).map((row, i) => {
            // Place each row at its pitch and indent it by depth.
            const top = (first + i) * rowHeight
            const indent = { paddingLeft: 8 + row.depth * 16 }
            if (row.type === 'dir') {
              return (
                <div
                  key={row.dir.path}
                  className="row dir"
                  role="treeitem"
                  aria-expanded={row.open}
                  style={{ top, ...indent }}
                  onClick={() =>
                    dispatch({ type: 'toggle', path: row.dir.path })
                  }
                >
                  <span className="twist" data-open={row.open}>
                    <Icon name="chevron" />
                  </span>
                  <Icon name="folder" />
                  <span className="name">{row.dir.name}</span>
                  <span className="meta">
                    {row.dir.counts[outliner.filter].toLocaleString()}
                  </span>
                </div>
              )
            }
            const path = tree.paths[row.file]!
            return (
              <div
                key={path}
                className="row file"
                role="treeitem"
                aria-selected={path === props.selected}
                style={{ top, ...indent }}
                title={path}
                onClick={() => props.onSelect(path)}
              >
                <span className="twist" />
                <Icon name={kindOf(path)} />
                <span className="name">
                  <bdi>{searching ? path : tree.names[row.file]}</bdi>
                </span>
                <span className="meta">{size(tree.sizes[row.file]!)}</span>
              </div>
            )
          })}
        </div>
      </div>
    </>
  )
}
