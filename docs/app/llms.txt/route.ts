import { docsLlms } from '@/lib/source'

export const revalidate = false

/** GET serves the index of pages for language models. */
export function GET() {
  return new Response(docsLlms.index())
}
