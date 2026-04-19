import { notFound } from "next/navigation"

import { PostEditor } from "@/components/admin/post-editor"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Badge } from "@/components/ui/badge"
import { requireAdmin } from "@/lib/auth"
import { getPostBySlug } from "@/lib/blog"

type EditAdminPostPageProps = {
  params: Promise<{
    slug: string
  }>
  searchParams: Promise<{
    saved?: string
  }>
}

export default async function EditAdminPostPage({
  params,
  searchParams,
}: EditAdminPostPageProps) {
  const { slug } = await params
  await requireAdmin(`/admin/posts/${slug}/edit`)

  const post = await getPostBySlug(slug)
  const { saved } = await searchParams

  if (!post) {
    notFound()
  }

  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-7xl flex-col gap-8 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <div className="space-y-3">
          <Badge variant="outline">Edit markdown post</Badge>
          <div className="space-y-2">
            <h1 className="font-heading text-4xl leading-none tracking-[-0.04em]">
              Edit {post.title}
            </h1>
            <p className="max-w-2xl text-base leading-7 text-muted-foreground">
              Update the metadata, edit the markdown, and save back to the
              dispatch store.
            </p>
          </div>
        </div>

        {saved ? (
          <div className="rounded-xl border border-emerald-500/30 bg-emerald-500/10 p-4 text-sm text-emerald-900 dark:text-emerald-100">
            Post saved.
          </div>
        ) : null}

        <PostEditor
          mode="edit"
          initialValues={{
            previousSlug: post.slug,
            slug: post.slug,
            title: post.title,
            kicker: post.kicker,
            excerpt: post.excerpt,
            intro: post.intro,
            publishedAt: post.publishedAt,
            tags: post.tags.join(", "),
            content: post.content,
          }}
        />
      </div>
      <SiteFooter />
    </main>
  )
}
