// The parts of the game's Panorama script API the renderer uses.

/** Panel is one panel in the game's interface. */
interface Panel {
  readonly id: string
  readonly paneltype: string
  hittest: boolean
  text: string
  readonly style: Record<string, string>
  IsValid(): boolean
  GetParent(): Panel | null
  GetChildCount(): number
  GetChild(index: number): Panel | null
  MoveChildBefore(child: Panel, before: Panel): void
  MoveChildAfter(child: Panel, after: Panel): void
  SetImage(path: string): void
  SetURL(url: string): void
  SetPanelEvent(event: string, handler: () => void): void
  FindChildTraverse(id: string): Panel | null
  DeleteAsync(seconds: number): void
}

/** $ is the Panorama script API. */
declare const $: {
  Msg(...values: unknown[]): void
  GetContextPanel(): Panel
  CreatePanel(type: string, parent: Panel, id: string): Panel
  RegisterEventHandler(event: string, panel: Panel, handler: (...args: never[]) => boolean | void): void
  Schedule(seconds: number, callback: () => void): number
  CancelScheduled(handle: number): void
  DispatchEvent(event: string, ...args: string[]): void
}
