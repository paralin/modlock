// The renderer sends a draft only after Submit. Small paced console lines
// take the same player-authenticated route as the short entry. The host
// calls the mod only after the final line completes the whole document.

import type { DocumentInput } from '../../src/wasm/ui/ui.pb.js'
import { base64Url, utf8 } from './data.js'
import type { Panel, Panorama } from './draw.js'
import type { MenuHost } from './menu.js'

/** maxDocument is the byte limit for input and output, including newlines. */
export const maxDocument = 262144

/** documentProperties gives stock multiline entries one sentinel character
 * beyond the byte limit, so a pasted value cannot look valid after clipping. */
export const documentProperties = { multiline: 'true', maxchars: '262145' }

/** Form is one local draft and the host control lifetime that can accept it. */
export interface Form {
  panel: Panel
  entry: Panel
  submit: Panel
  label: Panel
  status: Panel
  prefix: string
  live: (form: Form) => boolean
}

/** Transfer keeps a fixed snapshot of the draft submitted by the player. */
interface Transfer {
  form: Form
  prefix: string
  bytes: number[]
  sent: number
}

/** Documents manages the renderer's one outgoing document. */
export class Documents {
  private active?: Transfer
  private serial = 0

  constructor(
    private readonly api: Panorama,
    private readonly host?: MenuHost,
  ) {}

  /** form updates the controls without touching a draft during ordinary redraws. */
  form(
    panel: Panel,
    mod: string,
    node: string,
    config: DocumentInput,
    disabled: boolean,
    old: Form | undefined,
    live: Form['live'],
  ): Form {
    // Only the host-stamped control can start a transfer.
    const prefix =
      config.control &&
      /^[a-z0-9]+(-[a-z0-9]+)*$/.test(mod) &&
      /^[\w.:-]{1,64}$/.test(node)
        ? `modlockinput ${mod} ${node} d:${config.control}:`
        : ''
    const form = old ?? this.create(panel, config.placeholder ?? '', live)
    if (form.prefix !== prefix) {
      if (this.active?.form === form) this.cancel()
      form.entry.text = ''
      form.status.text = ''
      form.prefix = prefix
    }
    form.live = live
    form.entry.enabled = !!prefix && !disabled
    form.submit.enabled = form.entry.enabled && this.active?.form !== form
    form.label.text = config.submitLabel || 'Submit'
    return form
  }

  private create(panel: Panel, placeholder: string, live: Form['live']): Form {
    // Use the same multiline control as the game uses for importing settings.
    const entry = this.api.CreatePanel(
      'TextEntry',
      panel,
      'ModlockDocumentDraft',
      { ...documentProperties, placeholder },
    )
    const submit = this.api.CreatePanel(
      'Button',
      panel,
      'ModlockDocumentSubmit',
    )
    const label = this.api.CreatePanel(
      'Label',
      submit,
      'ModlockDocumentSubmitLabel',
    )
    const status = this.api.CreatePanel('Label', panel, 'ModlockDocumentStatus')

    // Give the entry room to show multiple lines and the button its own row.
    Object.assign(entry.style, {
      width: '100%',
      height: '130px',
      padding: '8px',
      fontSize: '18px',
      color: '#ffefd7',
      backgroundColor: '#1a1a1a',
      border: '1px solid #847c6d',
    })
    Object.assign(submit.style, {
      padding: '8px 16px',
      backgroundColor: '#39342d',
    })
    for (const each of [label, status]) {
      each.hittest = false
      each.style.color = '#ffefd7'
      each.style.fontSize = '18px'
    }

    // Only activating this button sends the draft, not native edit events.
    const form = { panel, entry, submit, label, status, prefix: '', live }
    submit.SetPanelEvent('onactivate', () => this.send(form))
    return form
  }

  /** check drops a submission whose form or session stopped accepting it. */
  check(): void {
    const form = this.active?.form
    if (form && !this.allowed(form)) this.cancel()
  }

  /** dismiss cancels a pending draft under the menu being closed. */
  dismiss(content: Panel): void {
    for (
      let panel = this.active?.form.panel;
      panel;
      panel = panel.GetParent?.() ?? undefined
    ) {
      if (panel === content) {
        this.cancel()
        return
      }
    }
  }

  /** cancel tells the host to discard the parts it has and stops local sends. */
  cancel(): void {
    // Stop scheduled sends before dispatching the cancel frame.
    const sending = this.active
    if (!sending) return
    this.active = undefined
    this.command(`${sending.prefix}x`)
    if (sending.form.panel.IsValid()) {
      sending.form.submit.enabled = sending.form.entry.enabled
      sending.form.status.text = 'Not sent. Submit again to retry.'
    }
  }

  private allowed(form: Form): boolean {
    return (
      form.panel.IsValid() &&
      form.entry.enabled &&
      form.live(form) &&
      (this.host?.allowed() ?? true) &&
      !this.host?.stock()
    )
  }

  private send(form: Form): void {
    // Validate the whole draft before sending its declared length.
    if (
      !this.api.Schedule ||
      this.active ||
      !form.submit.enabled ||
      !this.allowed(form)
    )
      return
    const text = form.entry.text
    const bytes = utf8(text, maxDocument + 4)
    if (text.includes('\0') || /[\uD800-\uDFFF]/u.test(text)) {
      form.status.text = 'Use valid text up to 256 KiB.'
      return
    }

    // Take a snapshot so later edits cannot change the in-flight bytes.
    this.serial = Math.max(Date.now(), this.serial + 1)
    const prefix = `${form.prefix}${this.serial}:`

    // Declare a draft past the limit without its bytes, so the host refuses
    // it and the mod hears why.
    if (bytes.length > maxDocument) {
      this.command(`${prefix}b:${bytes.length}`)
      form.status.text = 'Use valid text up to 256 KiB.'
      return
    }
    const sending: Transfer = { form, prefix, bytes, sent: 0 }
    this.active = sending
    form.submit.enabled = false
    form.status.text = 'Sending…'
    this.command(`${prefix}b:${bytes.length}`)
    this.api.Schedule(0.05, () => this.advance(sending))
  }

  private advance(sending: Transfer): void {
    // Stop if the draft closed while this batch was scheduled.
    if (this.active !== sending) return
    if (!this.allowed(sending.form)) {
      this.cancel()
      return
    }
    const { prefix, bytes, form } = sending

    // Leave room for the command, ids and framing below the console limit.
    for (let i = 0; i < 4 && sending.sent < bytes.length; i++) {
      const chunk = bytes.slice(sending.sent, sending.sent + 180)
      this.command(`${prefix}p:${sending.sent}:${base64Url(chunk)}`)
      sending.sent += chunk.length
    }
    if (sending.sent === bytes.length) {
      this.active = undefined
      this.command(`${prefix}e`)
      form.submit.enabled = form.entry.enabled
      form.status.text = 'Sent. If nothing changes, submit again.'
    } else {
      this.api.Schedule?.(0.05, () => this.advance(sending))
    }
  }

  private command(line: string): void {
    this.api.DispatchEvent('CitadelConCommand', line)
  }
}
