import { PostEditor } from "@/components/admin/post-editor"
import { SiteFooter } from "@/components/site-footer"
import { SiteHeader } from "@/components/site-header"
import { Badge } from "@/components/ui/badge"
import { requireAdmin } from "@/lib/auth"

export default async function NewAdminPostPage() {
  await requireAdmin("/admin/posts/new")

  return (
    <main className="min-h-svh">
      <SiteHeader />
      <div className="mx-auto flex w-full max-w-7xl flex-col gap-8 px-6 pt-10 pb-20 md:px-8 lg:pt-14">
        <div className="space-y-3">
          <Badge variant="outline">New markdown post</Badge>
          <div className="space-y-2">
            <h1 className="font-heading text-4xl leading-none tracking-[-0.04em]">
              Create a new blog entry
            </h1>
            <p className="max-w-2xl text-base leading-7 text-muted-foreground">
              Write in markdown, preview as you go, and save directly into the
              Black Lantern dispatch store.
            </p>
          </div>
        </div>
        <PostEditor
          mode="create"
          initialValues={{
            slug: "",
            title: "",
            kicker: "Post",
            excerpt: "",
            intro: "",
            publishedAt: new Date().toISOString().slice(0, 10),
            tags: "",
            content: "# New dispatch\n\nStart writing by lantern light.",
          }}
        />
      </div>
      <SiteFooter />
    </main>
  )
}
