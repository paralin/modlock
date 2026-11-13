import { docsLlms } from '@/lib/source'

export const revalidate = false

/** GET serves every page as one Markdown document. */
export async function GET() {
  return new Response(await docsLlms.full())
}
