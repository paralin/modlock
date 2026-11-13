import {
  DocsBody,
  DocsDescription,
  DocsPage,
  DocsTitle,
  MarkdownCopyButton,
  ViewOptionsPopover,
} from 'fumadocs-ui/layouts/docs/page'
import { createRelativeLink } from 'fumadocs-ui/mdx'
import type { Metadata } from 'next'
import { notFound } from 'next/navigation'

import { getMDXComponents } from '@/components/mdx'
import {
  basePath,
  markdownRoute,
  markdownSegments,
  repository,
  repositoryUrl,
} from '@/lib/site'
import { source } from '@/lib/source'

export default async function Page(props: PageProps<'/[[...slug]]'>) {
  // A path with no page is the not-found page.
  const { slug } = await props.params
  const page = source.getPage(slug)
  if (!page) {
    notFound()
  }

  // The copy and view controls read the page's plain text copy, and the edit
  // link opens its source on GitHub.
  const MDX = page.data.body
  const markdownUrl = [
    basePath + markdownRoute,
    ...markdownSegments(page),
  ].join('/')
  const editUrl = `${repositoryUrl}/blob/${repository.branch}/${repository.contentDir}/${encodeURI(page.path)}`

  // The page draws its title, the controls and then the body.
  return (
    <DocsPage toc={page.data.toc} full={page.data.full}>
      <DocsTitle>{page.data.title}</DocsTitle>
      <DocsDescription className="mb-0">
        {page.data.description}
      </DocsDescription>
      <div className="flex flex-row items-center gap-2 border-b pb-6">
        <MarkdownCopyButton markdownUrl={markdownUrl} />
        <ViewOptionsPopover markdownUrl={markdownUrl} githubUrl={editUrl} />
      </div>
      <DocsBody>
        <MDX
          components={getMDXComponents({ a: createRelativeLink(source, page) })}
        />
      </DocsBody>
    </DocsPage>
  )
}

export function generateStaticParams() {
  return source.generateParams()
}

export async function generateMetadata(
  props: PageProps<'/[[...slug]]'>,
): Promise<Metadata> {
  const { slug } = await props.params
  const page = source.getPage(slug)
  if (!page) {
    notFound()
  }
  return { title: page.data.title, description: page.data.description }
}
