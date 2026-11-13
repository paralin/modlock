import { notFound } from 'next/navigation'

import { markdownSegments } from '@/lib/site'
import { docsLlms, source } from '@/lib/source'

export const revalidate = false

/** GET serves one page as Markdown, at the page's path plus content.md. */
export async function GET(
  _req: Request,
  { params }: RouteContext<'/llms.mdx/[[...slug]]'>,
) {
  const { slug } = await params
  const page = source.getPage(slug?.slice(0, -1))
  if (!page) {
    notFound()
  }
  return new Response(await docsLlms.page(page), {
    headers: { 'Content-Type': 'text/markdown' },
  })
}

export function generateStaticParams() {
  return source.getPages().map((page) => ({ slug: markdownSegments(page) }))
}
