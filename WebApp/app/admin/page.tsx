import Link from "next/link"
import { Plus, Settings } from "lucide-react"

import { deletePostAction, logoutAction } from "@/app/admin/actions"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardDescription,
  CardFooter,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { requireAdmin } from "@/lib/auth"
import { getAllPosts, getBlogStorageInfo } from "@/lib/blog"

type AdminDashboardPageProps = {
  searchParams: Promise<{
    deleted?: string
  }>
}

export default async function AdminDashboardPage({
  searchParams,
}: AdminDashboardPageProps) {
  await requireAdmin("/admin")
  const posts = await getAllPosts()
  const storage = await getBlogStorageInfo()
  const { deleted } = await searchParams

  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-6xl flex-col gap-8 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <div className="flex flex-wrap items-end justify-between gap-4">
          <div className="space-y-3">
            <Badge variant="outline">Admin dashboard</Badge>
            <div className="space-y-2">
              <h1 className="font-heading text-4xl leading-none tracking-[-0.04em]">
                Manage Black Lantern content
              </h1>
              <p className="max-w-2xl text-base leading-7 text-muted-foreground">
                Dispatches are markdown files. Create a new post, edit an
                existing one, or delete content directly from here.
              </p>
            </div>
          </div>
          <div className="flex flex-wrap gap-3">
            <Button asChild size="lg">
              <Link href="/admin/posts/new">
                <Plus />
                New post
              </Link>
            </Button>
            <form action={logoutAction}>
              <Button variant="outline" size="lg" type="submit">
                Sign out
              </Button>
            </form>
          </div>
        </div>

        {deleted ? (
          <div className="rounded-xl border border-emerald-500/30 bg-emerald-500/10 p-4 text-sm text-emerald-900 dark:text-emerald-100">
            Post deleted.
          </div>
        ) : null}

        <div className="grid gap-4 xl:grid-cols-[minmax(0,1.2fr)_320px]">
          <div className="grid gap-4">
            {posts.length === 0 ? (
              <Card className="border border-foreground/10 bg-card/85">
                <CardHeader>
                  <CardTitle>No posts yet</CardTitle>
                  <CardDescription>
                    Start with the new post editor and the blog will populate
                    automatically.
                  </CardDescription>
                </CardHeader>
              </Card>
            ) : (
              posts.map((post) => (
                <Card
                  key={post.slug}
                  className="border border-foreground/10 bg-card/85"
                >
                  <CardHeader className="space-y-4">
                    <div className="flex flex-wrap items-center justify-between gap-3">
                      <Badge variant="outline">{post.kicker}</Badge>
                      <span className="font-mono text-[11px] tracking-[0.18em] text-muted-foreground uppercase">
                        {post.publishedLabel} / {post.readingTime}
                      </span>
                    </div>
                    <div className="space-y-2">
                      <CardTitle className="text-lg leading-tight">
                        {post.title}
                      </CardTitle>
                      <CardDescription>{post.excerpt}</CardDescription>
                    </div>
                  </CardHeader>
                  <CardContent className="flex flex-wrap gap-2">
                    {post.tags.map((tag) => (
                      <Badge key={tag} variant="secondary">
                        {tag}
                      </Badge>
                    ))}
                  </CardContent>
                  <CardFooter className="flex flex-wrap justify-between gap-3 border-foreground/10">
                    <div className="flex flex-wrap gap-2">
                      <Button asChild>
                        <Link href={`/admin/posts/${post.slug}/edit`}>
                          Edit
                        </Link>
                      </Button>
                      <Button asChild variant="outline">
                        <Link href={`/blog/${post.slug}`}>View</Link>
                      </Button>
                    </div>
                    <form action={deletePostAction}>
                      <input type="hidden" name="slug" value={post.slug} />
                      <Button variant="destructive" type="submit">
                        Delete
                      </Button>
                    </form>
                  </CardFooter>
                </Card>
              ))
            )}
          </div>
          <Card className="border border-foreground/10 bg-background/80">
            <CardHeader>
              <div className="flex items-center gap-2 text-muted-foreground">
                <Settings className="size-4" />
                <span className="font-mono text-[11px] tracking-[0.18em] uppercase">
                  Current controls
                </span>
              </div>
              <CardTitle>What this admin area can do</CardTitle>
              <CardDescription>
                The foundation is in place for broader site controls, but blog
                publishing is fully editable now.
              </CardDescription>
            </CardHeader>
            <CardContent className="space-y-3 text-sm leading-7 text-muted-foreground">
              <p>Create and edit markdown posts from the browser.</p>
              <p>Publish to the public blog routes immediately after save.</p>
              <p>Delete old posts without manually touching the filesystem.</p>
              <p>
                Current storage:{" "}
                <code>{storage.storageDir}</code>
              </p>
              <p>
                Seed source: <code>{storage.seedDir}</code>
              </p>
              <p>
                Persistence:{" "}
                {storage.isUsingPersistentStorage
                  ? "external volume or custom storage path detected"
                  : "local app directory"}
              </p>
              <p>
                Grow this section later for podcast links, home page content, or
                other rotation controls.
              </p>
            </CardContent>
          </Card>
        </div>
      </div>
      <SiteFooter />
    </main>
  )
}
