// The archive's directory tree, built once from the Listing, and the rows
// the outliner shows for the folders open and the kind and search chosen.
import type { Listing } from '../studio.pb.js'
import { kindOf, type Kind } from './api.js'

// Filter is the kind of file the outliner shows; all shows every file.
export type Filter = Kind | 'all'

// Dir is one directory of the archive.
export interface Dir {
  // path is the directory's archive path, empty for the root.
  path: string
  name: string
  dirs: Dir[]
  // files are the directory's own files, by index in Tree.paths.
  files: number[]
  // counts are how many files of each filter lie under the directory.
  counts: Record<Filter, number>
}

// Tree is the archive's directories and every file's path.
export interface Tree {
  root: Dir
  // paths are every file's archive path, by file index.
  paths: string[]
  // names are every file's name within its directory, by file index.
  names: string[]
  // sizes are every file's size in bytes, by file index.
  sizes: number[]
}

// emptyCounts returns zero counts for every filter.
const emptyCounts = (): Record<Filter, number> => ({
  all: 0,
  map: 0,
  model: 0,
  texture: 0,
  material: 0,
  data: 0,
})

// buildTree builds the tree from the server's listing.
export function buildTree(listing: Listing): Tree {
  // Make each directory, with every ancestor the listing leaves out.
  const root: Dir = {
    path: '',
    name: '',
    dirs: [],
    files: [],
    counts: emptyCounts(),
  }
  const byPath = new Map<string, Dir>([['', root]])
  const ensure = (path: string): Dir => {
    // Make the parent first and add this directory to it.
    const found = byPath.get(path)
    if (found) return found
    const cut = path.lastIndexOf('/')
    const parent = ensure(cut < 0 ? '' : path.slice(0, cut))
    const dir: Dir = {
      path,
      name: path.slice(cut + 1),
      dirs: [],
      files: [],
      counts: emptyCounts(),
    }
    parent.dirs.push(dir)
    byPath.set(path, dir)
    return dir
  }
  const dirs = (listing.dirs ?? []).map(ensure)

  // Place each file and count it in every directory above it.
  const tree: Tree = { root, paths: [], names: [], sizes: [] }
  for (const file of listing.files ?? []) {
    const dir = dirs[file.dir ?? 0] ?? root
    const name = file.name ?? ''
    const index = tree.paths.length
    tree.paths.push(dir.path ? `${dir.path}/${name}` : name)
    tree.names.push(name)
    tree.sizes.push(Number(file.size ?? 0))
    dir.files.push(index)
    const kind = kindOf(name)
    for (let up: string | null = dir.path; up !== null; up = parentOf(up)) {
      const counts = byPath.get(up)!.counts
      counts.all++
      counts[kind]++
    }
  }

  // Sort folders by name; files arrive sorted.
  const sort = (dir: Dir) => {
    dir.dirs.sort((a, b) => (a.name < b.name ? -1 : 1))
    dir.dirs.forEach(sort)
  }
  sort(root)
  return tree
}

// parentOf returns a directory's parent path, or null for the root.
export function parentOf(path: string): string | null {
  if (!path) return null
  const cut = path.lastIndexOf('/')
  return cut < 0 ? '' : path.slice(0, cut)
}

// ancestors returns the directories that hold an archive path, outermost
// first, without the root.
export function ancestors(path: string): string[] {
  const out: string[] = []
  for (let up = parentOf(path); up; up = parentOf(up)) out.unshift(up)
  return out
}

// Row is one row of the outliner: a folder or a file.
export type Row =
  | { type: 'dir'; dir: Dir; depth: number; open: boolean }
  | { type: 'file'; file: number; depth: number }

// treeRows returns the rows of the open folders, showing only folders and
// files of the filter.
export function treeRows(
  tree: Tree,
  open: ReadonlySet<string>,
  filter: Filter,
): Row[] {
  const rows: Row[] = []
  const walk = (dir: Dir, depth: number) => {
    // Folders first, then the folder's own files.
    for (const child of dir.dirs) {
      if (child.counts[filter] === 0) continue
      const isOpen = open.has(child.path)
      rows.push({ type: 'dir', dir: child, depth, open: isOpen })
      if (isOpen) walk(child, depth + 1)
    }
    for (const file of dir.files) {
      if (filter === 'all' || kindOf(tree.names[file]!) === filter)
        rows.push({ type: 'file', file, depth })
    }
  }
  walk(tree.root, 0)
  return rows
}

// searchRows returns every file of the filter whose path holds each word of
// the query, as flat rows.
export function searchRows(tree: Tree, query: string, filter: Filter): Row[] {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean)
  const rows: Row[] = []
  for (const [file, path] of tree.paths.entries()) {
    if (filter !== 'all' && kindOf(path) !== filter) continue
    if (words.every((word) => path.includes(word)))
      rows.push({ type: 'file', file, depth: 0 })
  }
  return rows
}
