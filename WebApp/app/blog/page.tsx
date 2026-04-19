import Link from "next/link"
import { ArrowLeft, ArrowRight } from "lucide-react"

import { SectionHeading } from "@/components/section-heading"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { getAllPosts } from "@/lib/blog"

export default async function BlogPage() {
  const blogPosts = await getAllPosts()

  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-5xl flex-col gap-10 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <div className="flex flex-wrap items-center justify-between gap-4">
          <SectionHeading
            eyebrow="Dispatch Archive"
            title="A simpler archive view."
            description="Everything kept in a single column so the writing is easier to scan and easier to review."
          />
          <Button asChild variant="outline">
            <Link href="/">
              <ArrowLeft />
              Back to lantern
            </Link>
          </Button>
        </div>

        <div className="space-y-4">
          {blogPosts.map((post) => (
            <article
              key={post.slug}
              className="rounded-[1.25rem] border border-white/10 bg-card/50 p-5"
            >
              <div className="flex flex-wrap items-center justify-between gap-3">
                <div className="flex flex-wrap items-center gap-2">
                  <Badge
                    variant="outline"
                    className="border-primary/25 text-primary"
                  >
                    {post.kicker}
                  </Badge>
                  <span className="font-mono text-[11px] tracking-[0.18em] text-muted-foreground uppercase">
                    {post.publishedLabel}
                  </span>
                </div>
                <span className="font-mono text-[11px] tracking-[0.18em] text-muted-foreground uppercase">
                  {post.readingTime}
                </span>
              </div>

              <div className="mt-4 space-y-3">
                <h2 className="font-heading text-3xl leading-none text-foreground">
                  {post.title}
                </h2>
                <p className="max-w-3xl text-sm leading-7 text-muted-foreground">
                  {post.excerpt}
                </p>
              </div>

              <div className="mt-4 flex flex-wrap items-center justify-between gap-3">
                <div className="flex flex-wrap gap-2">
                  {post.tags.map((tag) => (
                    <Badge key={tag} variant="secondary">
                      {tag}
                    </Badge>
                  ))}
                </div>
                <Button asChild variant="ghost">
                  <Link href={`/blog/${post.slug}`}>
                    Read dispatch
                    <ArrowRight />
                  </Link>
                </Button>
              </div>
            </article>
          ))}
        </div>
      </div>
      <SiteFooter />
    </main>
  )
}
