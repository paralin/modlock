// Studio's icons: 16 pixel outline glyphs drawn in the text's color.
import type { ReactNode } from 'react'

// glyph wraps an icon's strokes in a 16 pixel box.
function glyph(children: ReactNode) {
  return (
    <svg
      className="icon"
      width="16"
      height="16"
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.25"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {children}
    </svg>
  )
}

// Icon names every glyph.
export type IconName =
  | 'chevron'
  | 'folder'
  | 'map'
  | 'model'
  | 'texture'
  | 'material'
  | 'data'
  | 'search'
  | 'solid'
  | 'wireframe'
  | 'frame'
  | 'grid'
  | 'collision'

// paths are each glyph's strokes.
const paths: Record<IconName, ReactNode> = {
  chevron: <path d="M6 4l4 4-4 4" />,
  folder: <path d="M2 4.5h4l1.5 1.5H14v6.5H2z" />,
  map: (
    <>
      <path d="M2 4l4-1.5 4 1.5 4-1.5v9.5L10 13.5 6 12l-4 1.5z" />
      <path d="M6 2.5V12M10 4v9.5" />
    </>
  ),
  model: (
    <>
      <path d="M8 2l5.5 3v6L8 14l-5.5-3V5z" />
      <path d="M2.5 5L8 8l5.5-3M8 8v6" />
    </>
  ),
  texture: (
    <>
      <rect x="2.5" y="2.5" width="11" height="11" rx="1" />
      <path d="M2.5 11l3.5-3.5 3 3 2-2 2.5 2.5" />
      <circle cx="10.5" cy="5.5" r="1" />
    </>
  ),
  material: (
    <>
      <circle cx="8" cy="8" r="5.5" />
      <path d="M5 6.5a3.5 3.5 0 0 1 3-2" />
    </>
  ),
  data: (
    <>
      <path d="M4 2.5h5.5l2.5 2.5v8.5H4z" />
      <path d="M6 8h4M6 10.5h4" />
    </>
  ),
  search: (
    <>
      <circle cx="7" cy="7" r="4" />
      <path d="M10 10l3.5 3.5" />
    </>
  ),
  solid: <circle cx="8" cy="8" r="5.5" fill="currentColor" stroke="none" />,
  wireframe: (
    <>
      <circle cx="8" cy="8" r="5.5" />
      <ellipse cx="8" cy="8" rx="2.5" ry="5.5" />
      <path d="M2.5 8h11" />
    </>
  ),
  frame: (
    <>
      <path d="M2.5 5.5v-3h3M10.5 2.5h3v3M13.5 10.5v3h-3M5.5 13.5h-3v-3" />
      <circle cx="8" cy="8" r="2" />
    </>
  ),
  grid: <path d="M2.5 5.5h11M2.5 10.5h11M5.5 2.5v11M10.5 2.5v11" />,
  collision: (
    <>
      <path d="M8 2l5.5 3v6L8 14l-5.5-3V5z" strokeDasharray="2 1.5" />
      <path d="M5.5 6.5l2.5 1.5 2.5-1.5M8 8v3" />
    </>
  ),
}

// Icon draws one glyph.
export function Icon(props: { name: IconName }) {
  return glyph(paths[props.name])
}
