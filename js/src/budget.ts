// The frame's budget: the time the host gives the mod each frame, which the
// library starts before the mod's frame handlers. late reads it; tasks.ts
// paces tasks by it.
//
// This module holds no promise, so a mod that never starts a task stays free
// of async code, which scriptc's library mode requires. The library resumes
// tasks through a hook that the first task installs.

/** deadline is the runtime clock reading past which the frame is late. */
let deadline = Infinity

/** resume resumes the tasks waiting for this frame, once a task exists. */
let resume: (() => void) | undefined

/**
 * late reports whether the current frame is past the time the host gave the
 * mod. It is false whenever the host gave none.
 */
export function late(): boolean {
  return performance.now() > deadline
}

/**
 * beginFrame starts the frame's budget, in microseconds, zero for none. The
 * library calls it before the frame's handlers, so they count against it.
 */
export function beginFrame(budgetMicroseconds: number): void {
  deadline =
    budgetMicroseconds === 0
      ? Infinity
      : performance.now() + budgetMicroseconds / 1000
}

/** endFrame resumes the tasks that waited for this frame. */
export function endFrame(): void {
  resume?.()
}

/** onEndFrame sets the hook endFrame runs; tasks.ts sets it once. */
export function onEndFrame(hook: () => void): void {
  resume = hook
}
