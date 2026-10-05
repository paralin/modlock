// The bridge reads the local player's screen from the controller on loopback.
// Deadlock loads only https pages into HTML panels, so the panel loads a relay
// page from an https origin that reaches the controller and streams each
// screen into its title in acknowledged parts.

import { Screen } from '../../proto/modlock/ui.pb.js'

/** titlePrefix marks a part of a screen in the state page's title. */
const titlePrefix = 'modlock:snapshot:'

/** loadDeadline is how long a page may take to load before it is loaded again. */
const loadDeadline = 10

/** Bridge connects the renderer to the controller through a relay page. */
export class Bridge {
  /** parts holds the parts of the screen arriving now. */
  private parts: { id: number; total: number; text: string } | null = null
  /** lastTitle is the last part read; navigating to a fragment repeats it. */
  private lastTitle = ''
  /** loading is the deadline of the state page's load, if it is loading. */
  private loading: number | undefined

  constructor(
    private readonly statePanel: Panel,
    private readonly stateURL: string,
    private readonly show: (screen: Screen | null) => void,
  ) {
    // Registering the navigation event lets the panel navigate.
    $.RegisterEventHandler('HTMLStartRequest', statePanel, () => false)
    $.RegisterEventHandler('HTMLTitle', statePanel, (_: never, title: string) => this.read(title))
    this.load()
  }

  /** load loads the state page, and loads it again if no title arrives in time. */
  private load(): void {
    this.statePanel.SetURL(this.stateURL)
    this.loading = $.Schedule(loadDeadline, () => {
      this.loading = undefined
      this.load()
    })
  }

  /** read takes one state title: a part of a screen, or the page's status. */
  private read(title: string): boolean {
    if (this.loading !== undefined) {
      $.CancelScheduled(this.loading)
      this.loading = undefined
    }

    // A status title means the stream is not delivering; drop the screen
    // when it ended.
    if (!title.startsWith(titlePrefix)) {
      this.parts = null
      this.lastTitle = ''
      if (title === '') {
        this.show(null)
      }
      return false
    }
    if (title === this.lastTitle) {
      return false
    }
    this.lastTitle = title

    // Join the part to the screen it belongs to, and acknowledge it.
    let screen: Screen
    try {
      const part = JSON.parse(title.slice(titlePrefix.length)) as { id: number; offset: number; total: number; chunk: string }
      if (part.offset === 0) {
        this.parts = { id: part.id, total: part.total, text: '' }
      }
      const parts = this.parts
      if (!parts || parts.id !== part.id || parts.text.length !== part.offset) {
        throw new Error('a part of the screen is missing')
      }
      parts.text += part.chunk
      const reply = encodeURIComponent(JSON.stringify({ frameAck: part.id, offset: parts.text.length }))
      this.statePanel.SetURL(`${this.stateURL}#${reply}`)
      if (parts.text.length < parts.total) {
        return false
      }
      this.parts = null
      screen = Screen.fromJsonString(decodeURIComponent(parts.text))
    } catch (error) {
      // The page times out the unacknowledged part and sends the screen again.
      $.Msg('[Modlock] could not read the screen: ', String(error))
      this.parts = null
      return false
    }
    try {
      this.show(screen)
    } catch (error) {
      $.Msg('[Modlock] could not draw the screen: ', String(error))
    }
    return false
  }
}
