import type { Metadata } from "next"
import Link from "next/link"
import { ArrowLeft, ArrowRight, BookOpenText } from "lucide-react"
import { notFound } from "next/navigation"

import { MarkdownRenderer } from "@/components/markdown-renderer"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { getPostBySlug } from "@/lib/blog"

type BlogPostPageProps = {
  params: Promise<{
    slug: string
  }>
}

export async function generateMetadata({
  params,
}: BlogPostPageProps): Promise<Metadata> {
  const { slug } = await params
  const post = await getPostBySlug(slug)

  if (!post) {
    return {
      title: "Post not found",
    }
  }

  return {
    title: post.title,
    description: post.excerpt,
  }
}

export default async function BlogPostPage({ params }: BlogPostPageProps) {
  const { slug } = await params
  const post = await getPostBySlug(slug)

  if (!post) {
    notFound()
  }

  return (
    <main className="min-h-svh">
      <SiteHeader />
      <article className="mx-auto flex w-full max-w-4xl flex-col gap-10 px-6 pt-8 pb-20 md:px-8 lg:pt-12">
        <div className="space-y-6">
          <div className="flex flex-wrap items-center justify-between gap-3">
            <Button asChild variant="outline">
              <Link href="/blog">
                <ArrowLeft />
                Back to blog
              </Link>
            </Button>
            <Badge variant="outline">{post.kicker}</Badge>
          </div>
          <div className="space-y-4">
            <div className="flex items-center gap-2 text-muted-foreground">
              <BookOpenText className="size-4" />
              <span className="font-mono text-[11px] tracking-[0.2em] uppercase">
                {post.publishedLabel} / {post.readingTime}
              </span>
            </div>
            <h1 className="font-heading text-4xl leading-none tracking-[-0.04em] text-balance sm:text-5xl">
              {post.title}
            </h1>
            <p className="max-w-3xl text-lg leading-8 text-muted-foreground">
              {post.intro}
            </p>
            <div className="flex flex-wrap gap-2">
              {post.tags.map((tag) => (
                <Badge key={tag} variant="secondary">
                  {tag}
                </Badge>
              ))}
            </div>
          </div>
        </div>

        <div className="rounded-2xl border border-foreground/10 bg-background/85 p-6 sm:p-8">
          <MarkdownRenderer content={post.content} />
        </div>

        <div className="flex flex-wrap items-center justify-between gap-4 border-t border-foreground/10 pt-6">
          <p className="max-w-2xl text-muted-foreground">
            The Black Lantern Rotation is meant to feel collected rather than
            rushed. Each dispatch is part of the same orbit, even when it is
            looking in a different direction.
          </p>
          <Button asChild>
            <Link href="/broadcasts">
              Continue to broadcasts
              <ArrowRight />
            </Link>
          </Button>
        </div>
      </article>
      <SiteFooter />
    </main>
  )
}
