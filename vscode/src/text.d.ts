// A style sheet imported with { type: 'text' } is its text, which the bundle
// inlines.
declare module '*.css' {
  const text: string
  export default text
}
