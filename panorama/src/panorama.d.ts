// The parts of the game's Panorama script API the renderer uses.

/** Panel is one panel in the game's interface. */
interface Panel {
  readonly id: string
  readonly paneltype: string
  hittest: boolean
  enabled: boolean
  text: string
  value: number
  readonly actuallayoutwidth: number
  readonly actualuiscale_x: number
  GetPositionWithinWindow(): { x: number; y: number }
  checked: boolean
  visible: boolean
  readonly style: Record<string, string | null>
  IsValid(): boolean
  GetParent(): Panel | null
  GetChildCount(): number
  GetChild(index: number): Panel | null
  Children(): Panel[]
  RemoveAndDeleteChildren(): void
  MoveChildBefore(child: Panel, before: Panel): void
  MoveChildAfter(child: Panel, after: Panel): void
  SetImage(path: string): void
  SetURL(url: string): void
  SetPanelEvent(event: string, handler: () => void): void
  ClearPanelEvent(event: string): void
  SetAttributeString(name: string, value: string): void
  GetAttributeString(name: string, fallback: string): string
  SetAttributeInt(name: string, value: number): void
  GetAttributeInt(name: string, fallback: number): number
  layoutfile: string
  BLoadLayout(
    path: string,
    overrideExisting: boolean,
    partial: boolean,
  ): boolean
  BLoadLayoutSnippet(snippetName: string): boolean
  BLoadSnippet(snippetName: string): boolean
  FindChild(id: string): Panel | null
  FindChildTraverse(id: string): Panel | null
  FindChildrenWithClassTraverse(name: string): Panel[]
  BHasClass(name: string): boolean
  SetHasClass(name: string, on: boolean): void
  ToggleClass(name: string): void
  SwitchClass(slot: string, name: string): void
  SetDraggable(on: boolean): void
  IsDraggable(): boolean
  BHasKeyFocus?(): boolean
  BHasDescendantKeyFocus?(): boolean
  SetFocus?(): void
  AddClass(name: string): void
  RemoveClass(name: string): void
  DeleteAsync(seconds: number): void
  SetDialogVariable(name: string, value: string): void
  SetDialogVariableInt(name: string, value: number): void
  SetDialogVariableFloat(name: string, value: number): void
  Data(): Record<string, unknown>
  html: boolean
  steamid: string
  SetHeroID(id: number): void
  SetHeroName(name: string): void
  heroid: number
  heroname: string
  AddOption(option: Panel): void
  SetSelected(id: string): void
  GetSelected(): Panel | null
}

/** $ is the Panorama script API. */
declare const $: {
  Msg(...values: unknown[]): void
  MousePosition(): { x: number; y: number }
  GetContextPanel(): Panel
  CreatePanel(
    type: string,
    parent: Panel,
    id: string,
    properties?: Record<string, string>,
  ): Panel
  RegisterEventHandler(
    event: string,
    panel: Panel,
    handler: (...args: never[]) => boolean | void,
  ): void
  RegisterForUnhandledEvent(
    event: string,
    handler: (...args: never[]) => boolean | void,
  ): number
  Schedule(seconds: number, callback: () => void): number
  CancelScheduled(handle: number): void
  DispatchEvent(event: string, ...args: unknown[]): void
  Localize(token: string, panel?: object): string
}

/** PlayUISoundEvent plays one of the game's interface sound events. */
declare function PlayUISoundEvent(sound: string): number
/** StopUISoundEvent stops a sound PlayUISoundEvent started. */
declare function StopUISoundEvent(playing: number): void
