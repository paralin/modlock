// The properties editor: what the selected file is, and the controls that
// change how the viewport draws it.
import type { Dispatch, ReactNode } from 'react'
import { formatName } from './scene.js'
import type { Asset, Event } from './state.js'

// Properties shows the selected asset's properties.
export function Properties(props: {
  asset: Asset
  dispatch: Dispatch<Event>
  onSelect: (name: string) => void
}) {
  const { asset } = props
  return (
    <section className="panel properties" aria-label="Properties">
      {asset.state === 'idle' && (
        <p className="panel-note">Select a file in the outliner.</p>
      )}
      {asset.state === 'loading' && <p className="panel-note">Loading…</p>}
      {asset.state === 'failed' && (
        <p className="panel-note">This file could not be read.</p>
      )}
      {asset.state === 'map' && (
        <MapProperties asset={asset} onSelect={props.onSelect} />
      )}
      {asset.state === 'model' && <ModelProperties {...props} asset={asset} />}
      {asset.state === 'texture' && <TextureProperties asset={asset} />}
      {asset.state === 'data' && (
        <DataProperties asset={asset} onSelect={props.onSelect} />
      )}
    </section>
  )
}

// byKey orders entries by their keys.
const byKey = (a: [string, unknown], b: [string, unknown]) =>
  a[0].localeCompare(b[0])

// Section is one titled group of properties.
function Section(props: { title: string; children: ReactNode }) {
  return (
    <section className="section">
      <h2>{props.title}</h2>
      {props.children}
    </section>
  )
}

// Field is one labeled value.
function Field(props: { label: string; children: ReactNode }) {
  return (
    <div className="field">
      <span className="label">{props.label}</span>
      <span className="value">{props.children}</span>
    </div>
  )
}

// Link opens another archive file.
function Link(props: { name: string; onSelect: (name: string) => void }) {
  const short = props.name.slice(props.name.lastIndexOf('/') + 1)
  return (
    <button
      className="link"
      title={props.name}
      onClick={() => props.onSelect(props.name)}
    >
      <bdi>{short}</bdi>
    </button>
  )
}

// ModelProperties shows a model's statistics, level of detail, mesh groups
// and materials.
function ModelProperties(props: {
  asset: Extract<Asset, { state: 'model' }>
  dispatch: Dispatch<Event>
  onSelect: (name: string) => void
}) {
  const { asset, dispatch } = props
  const lods = Math.max(1, asset.model.lods ?? 1)
  const materials = Object.entries(asset.model.materials ?? {}).sort(byKey)
  return (
    <>
      <Section title="Statistics">
        <Field label="Draw calls">{asset.stats?.drawCalls ?? '…'}</Field>
        <Field label="Vertices">
          {asset.stats?.vertices.toLocaleString() ?? '…'}
        </Field>
        <Field label="Triangles">
          {asset.stats
            ? Math.round(asset.stats.triangles).toLocaleString()
            : '…'}
        </Field>
      </Section>
      {lods > 1 && (
        <Section title="Level of detail">
          <div className="segmented" role="group" aria-label="Level of detail">
            {Array.from({ length: lods }, (_, lod) => (
              <button
                key={lod}
                aria-pressed={asset.lod === lod}
                onClick={() => dispatch({ type: 'lod', lod })}
              >
                {lod}
              </button>
            ))}
          </div>
        </Section>
      )}
      {asset.groups !== null && (
        <Section title="Mesh groups">
          {(asset.model.groups ?? []).map((group, bit) => (
            <label key={bit} className="check">
              <input
                type="checkbox"
                checked={((asset.groups! >> BigInt(bit)) & 1n) !== 0n}
                onChange={() => dispatch({ type: 'group', bit })}
              />
              <span title={group}>{groupName(group, bit)}</span>
            </label>
          ))}
        </Section>
      )}
      <Missing missing={asset.missing} onSelect={props.onSelect} />
      <Section title="Materials">
        {materials.map(([name, material]) => (
          <div key={name} className="material">
            <Link name={`${name}_c`} onSelect={props.onSelect} />
            <span className="dim">{material.shader}</span>
            {Object.entries(material.textures ?? {})
              .sort(byKey)
              .map(([param, texture]) => (
                <Field key={param} label={param}>
                  <Link name={texture} onSelect={props.onSelect} />
                </Field>
              ))}
          </div>
        ))}
      </Section>
    </>
  )
}

// MapProperties shows what a map places and how much of it has loaded.
function MapProperties(props: {
  asset: Extract<Asset, { state: 'map' }>
  onSelect: (name: string) => void
}) {
  const { world, loaded, stats } = props.asset
  const models = world.models?.length ?? 0
  return (
    <>
      <Section title="Statistics">
        <Field label="Models">
          {stats
            ? models.toLocaleString()
            : `${loaded.toLocaleString()} of ${models.toLocaleString()}`}
        </Field>
        <Field label="Placements">
          {(world.placements?.length ?? 0).toLocaleString()}
        </Field>
        <Field label="Draw calls">
          {stats?.drawCalls.toLocaleString() ?? '…'}
        </Field>
        <Field label="Triangles">
          {stats ? Math.round(stats.triangles).toLocaleString() : '…'}
        </Field>
      </Section>
      <Missing missing={props.asset.missing} onSelect={props.onSelect} />
    </>
  )
}

// Missing lists the textures that cannot be drawn, with why.
function Missing(props: {
  missing: { texture: string; reason: string }[]
  onSelect: (name: string) => void
}) {
  if (props.missing.length === 0) return null
  return (
    <Section title="Textures not drawn">
      {props.missing.map((m) => (
        <div key={m.texture} className="missing">
          <Link name={m.texture} onSelect={props.onSelect} />
          <span className="dim">{m.reason}</span>
        </div>
      ))}
    </Section>
  )
}

// groupName shows a mesh group's name. The compiler names a body group's
// choice `<group>_@<index>_#&<choice>`; this shows `<group>: <choice>`.
function groupName(group: string, bit: number): string {
  const choice = /^(.*)_@\d+_#&(.*)$/.exec(group)
  if (choice) return `${choice[1]}: ${choice[2]}`
  return group || `Group ${bit}`
}

// TextureProperties shows a texture's header.
function TextureProperties(props: {
  asset: Extract<Asset, { state: 'texture' }>
}) {
  const { texture, shown } = props.asset
  return (
    <Section title="Texture">
      <Field label="Size">
        {texture.width}×{texture.height}
        {(texture.depth ?? 1) > 1 && `×${texture.depth}`}
      </Field>
      <Field label="Format">{formatName(texture.format ?? 0)}</Field>
      <Field label="Mips">{texture.mips}</Field>
      {texture.cube && <Field label="Shape">Cube map</Field>}
      {shown && (
        <Field label="Showing">
          {shown.width}×{shown.height}
        </Field>
      )}
    </Section>
  )
}

// DataProperties shows a resource's blocks, and a material's shader and
// textures.
function DataProperties(props: {
  asset: Extract<Asset, { state: 'data' }>
  onSelect: (name: string) => void
}) {
  // A material's DATA block names its shader and textures.
  const material = materialOf(props.asset.resource.json ?? '')
  return (
    <>
      {material && (
        <Section title="Material">
          <Field label="Shader">{material.shader}</Field>
          {material.textures.map(([param, texture]) => (
            <Field key={param} label={param}>
              <Link name={texture} onSelect={props.onSelect} />
            </Field>
          ))}
        </Section>
      )}
      <Section title="Blocks">
        <div className="tags">
          {(props.asset.resource.blocks ?? []).map((tag, i) => (
            <span key={i} className="tag">
              {tag}
            </span>
          ))}
        </div>
      </Section>
    </>
  )
}

// materialOf reads a material's shader and texture parameters from its
// decoded blocks, or returns null for any other resource.
function materialOf(
  json: string,
): { shader: string; textures: [string, string][] } | null {
  // Parse the DATA block, if there is one.
  let data: {
    m_shaderName?: string
    m_textureParams?: { m_name?: string; m_pValue?: string }[]
  }
  try {
    data = (JSON.parse(json) as { DATA?: typeof data }).DATA ?? {}
  } catch {
    return null
  }
  if (!data.m_shaderName) return null

  // List the textures it names.
  const textures: [string, string][] = []
  for (const param of data.m_textureParams ?? []) {
    if (param.m_name && param.m_pValue)
      textures.push([param.m_name, `${param.m_pValue}_c`])
  }
  return { shader: data.m_shaderName, textures }
}
