import './global.css'

import { DocsLayout } from 'fumadocs-ui/layouts/docs'
import type { Metadata } from 'next'
import { Inter, JetBrains_Mono } from 'next/font/google'

import { LanguageSelect } from '@/components/language'
import { Logo } from '@/components/logo'
import { Provider } from '@/components/provider'
import { defaultLanguage, restoreScript } from '@/lib/language'
import { repositoryUrl, siteDescription, siteName } from '@/lib/site'
import { source } from '@/lib/source'

const inter = Inter({ subsets: ['latin'], variable: '--font-inter' })
const mono = JetBrains_Mono({ subsets: ['latin'], variable: '--font-code' })

export const metadata: Metadata = {
  title: { default: siteName, template: `%s | ${siteName}` },
  description: siteDescription,
}

export default function Layout({ children }: LayoutProps<'/'>) {
  return (
    <html
      lang="en"
      data-lang={defaultLanguage}
      className={`${inter.variable} ${mono.variable}`}
      suppressHydrationWarning
    >
      <head>
        <script dangerouslySetInnerHTML={{ __html: restoreScript }} />
      </head>
      <body className="flex min-h-screen flex-col">
        <Provider>
          <DocsLayout
            tree={source.getPageTree()}
            nav={{ title: <Logo /> }}
            githubUrl={repositoryUrl}
            sidebar={{ banner: <LanguageSelect /> }}
          >
            {children}
          </DocsLayout>
        </Provider>
      </body>
    </html>
  )
}
