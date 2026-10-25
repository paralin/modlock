// Studio's state machine: the archive, the outliner, the selected asset and
// the viewport's display options, moved only by the events below.
import type { Model, Resource, Texture, World } from '../studio.pb.js'
import type { Shading, Stats, WorldStats } from './scene.js'
import { ancestors, type Filter, type Tree } from './tree.js'

// Archive is the game archive's file list as it loads.
export type Archive =
  | { state: 'loading' }
  | { state: 'ready'; tree: Tree }
  | { state: 'gone'; message: string }

// Asset is the selected file and how far it has loaded.
export type Asset =
  | { state: 'idle' }
  | { state: 'loading'; name: string }
  | {
      state: 'model'
      name: string
      model: Model
      lod: number
      // groups is the mask of mesh groups shown, or null when the model
      // names none.
      groups: bigint | null
      // stats are what the viewport draws, null until the first draw.
      stats: Stats | null
      // missing are the textures that cannot be drawn, with why.
      missing: { texture: string; reason: string }[]
    }
  | {
      state: 'map'
      name: string
      world: World
      // loaded counts the models drawn so far.
      loaded: number
      // stats are what the map draws, null until every model has loaded.
      stats: WorldStats | null
      missing: { texture: string; reason: string }[]
    }
  | {
      state: 'texture'
      name: string
      texture: Texture
      // shown is the size of the mip drawn, null while it loads.
      shown: { width: number; height: number } | null
      // unavailable says why this browser cannot draw it, or is null.
      unavailable: string | null
    }
  | { state: 'data'; name: string; resource: Resource }
  | { state: 'failed'; name: string; message: string }
  // lost is a file whose drawing the browser dropped when the GPU reset; it
  // loads again once the GPU is back.
  | { state: 'lost'; name: string }

// Outliner is the outliner's search, filter and open folders.
export interface Outliner {
  query: string
  filter: Filter
  open: ReadonlySet<string>
  // reveal is the row the outliner scrolls into view, by archive path.
  reveal: string
}

// Studio is the whole page's state.
export interface Studio {
  archive: Archive
  outliner: Outliner
  asset: Asset
  shading: Shading
  grid: boolean
  // collision shows a model's collision shapes over its meshes; a model
  // with only collision always shows it.
  collision: boolean
  // reset is the file the GPU last reset while drawing, so a second reset
  // on it fails instead of loading it again forever.
  reset: string
}

// initial is the page's state before anything loads.
export const initial: Studio = {
  archive: { state: 'loading' },
  outliner: { query: '', filter: 'model', open: new Set(), reveal: '' },
  asset: { state: 'idle' },
  shading: 'material',
  grid: true,
  collision: false,
  reset: '',
}

// Event moves the state.
export type Event =
  | { type: 'listed'; tree: Tree }
  | { type: 'unlisted'; message: string }
  | { type: 'query'; query: string }
  | { type: 'filter'; filter: Filter }
  | { type: 'toggle'; path: string }
  | { type: 'reveal'; path: string }
  | { type: 'select'; name: string }
  | { type: 'model'; name: string; model: Model }
  | { type: 'drawn'; name: string; stats: Stats }
  | { type: 'map'; name: string; world: World }
  | { type: 'progress'; name: string; loaded: number }
  | { type: 'built'; name: string; stats: WorldStats }
  | { type: 'missing'; name: string; texture: string; reason: string }
  | { type: 'lod'; lod: number }
  | { type: 'group'; bit: number }
  | { type: 'texture'; name: string; texture: Texture }
  | { type: 'shown'; name: string; width: number; height: number }
  | { type: 'unavailable'; name: string; reason: string }
  | { type: 'data'; name: string; resource: Resource }
  | { type: 'failed'; name: string; message: string }
  | { type: 'shading'; shading: Shading }
  | { type: 'grid' }
  | { type: 'collision' }
  | { type: 'lost' }
  | { type: 'restored' }

// reduce applies an event. A load's result for a file no longer selected is
// dropped.
export function reduce(studio: Studio, event: Event): Studio {
  switch (event.type) {
    case 'listed':
      return { ...studio, archive: { state: 'ready', tree: event.tree } }
    case 'unlisted':
      return { ...studio, archive: { state: 'gone', message: event.message } }
    case 'query':
    case 'filter':
    case 'toggle':
    case 'reveal':
      return { ...studio, outliner: reduceOutliner(studio.outliner, event) }
    case 'select':
      return {
        ...studio,
        asset: { state: 'loading', name: event.name },
        reset: '',
        outliner: reduceOutliner(studio.outliner, {
          type: 'reveal',
          path: event.name,
        }),
      }
    case 'shading':
      return { ...studio, shading: event.shading }
    case 'grid':
      return { ...studio, grid: !studio.grid }
    case 'collision':
      return { ...studio, collision: !studio.collision }
    case 'lod':
    case 'group':
      return { ...studio, asset: reduceView(studio.asset, event) }
    case 'lost': {
      const { asset } = studio
      if (asset.state === 'idle') return studio
      const { name } = asset
      if (studio.reset === name) {
        const message = 'The graphics card reset twice drawing this file.'
        return { ...studio, asset: { state: 'failed', name, message } }
      }
      return { ...studio, asset: { state: 'lost', name }, reset: name }
    }
    case 'restored': {
      const { asset } = studio
      if (asset.state !== 'lost') return studio
      return { ...studio, asset: { state: 'loading', name: asset.name } }
    }
  }
  return { ...studio, asset: reduceAsset(studio.asset, event) }
}

// reduceView changes which of the drawn model's meshes show.
function reduceView(
  asset: Asset,
  event: Extract<Event, { type: 'lod' | 'group' }>,
): Asset {
  if (asset.state !== 'model') return asset
  if (event.type === 'lod') return { ...asset, lod: event.lod }
  if (asset.groups === null) return asset
  return { ...asset, groups: asset.groups ^ (1n << BigInt(event.bit)) }
}

// reduceOutliner applies an outliner event.
function reduceOutliner(
  outliner: Outliner,
  event: Extract<Event, { type: 'query' | 'filter' | 'toggle' | 'reveal' }>,
): Outliner {
  switch (event.type) {
    case 'query':
      return { ...outliner, query: event.query }
    case 'filter':
      return { ...outliner, filter: event.filter }
    case 'toggle': {
      // Open or close one folder.
      const open = new Set(outliner.open)
      if (!open.delete(event.path)) open.add(event.path)
      return { ...outliner, open }
    }
    case 'reveal': {
      // Open every folder above the path and scroll to it.
      const open = new Set(outliner.open)
      for (const dir of ancestors(event.path)) open.add(dir)
      return { ...outliner, open, reveal: event.path }
    }
  }
}

// reduceAsset applies a load's result, which belongs to the selected file
// only.
function reduceAsset(asset: Asset, event: Event): Asset {
  // Drop a result for any other file.
  if (asset.state === 'idle' || !('name' in event)) return asset
  if (asset.name !== event.name) return asset

  // Move to the state the result names.
  const name = asset.name
  switch (event.type) {
    case 'model':
      return {
        state: 'model',
        name,
        model: event.model,
        lod: 0,
        groups: defaultGroups(event.model),
        stats: null,
        missing: [],
      }
    case 'drawn':
      return asset.state === 'model' ? { ...asset, stats: event.stats } : asset
    case 'map':
      return {
        state: 'map',
        name,
        world: event.world,
        loaded: 0,
        stats: null,
        missing: [],
      }
    case 'progress':
      return asset.state === 'map' ? { ...asset, loaded: event.loaded } : asset
    case 'built':
      return asset.state === 'map' ? { ...asset, stats: event.stats } : asset
    case 'missing':
      return asset.state === 'model' || asset.state === 'map'
        ? {
            ...asset,
            missing: [
              ...asset.missing,
              { texture: event.texture, reason: event.reason },
            ],
          }
        : asset
    case 'texture':
      return {
        state: 'texture',
        name,
        texture: event.texture,
        shown: null,
        unavailable: null,
      }
    case 'shown':
      return asset.state === 'texture'
        ? { ...asset, shown: { width: event.width, height: event.height } }
        : asset
    case 'unavailable':
      return asset.state === 'texture'
        ? { ...asset, unavailable: event.reason }
        : asset
    case 'data':
      return { state: 'data', name, resource: event.resource }
    case 'failed':
      return { state: 'failed', name, message: event.message }
  }
  return asset
}

// defaultGroups returns the mesh groups the game draws by default, or null
// for a model that names none.
function defaultGroups(model: Model): bigint | null {
  // An empty default mask means every group.
  const count = model.groups?.length ?? 0
  if (count === 0) return null
  const all = (1n << BigInt(count)) - 1n
  const defaults = model.defaultGroups ?? 0n
  return defaults === 0n ? all : defaults & all
}
