"use client"

import Link from "next/link"
import { useActionState, useMemo, useState } from "react"
import { useFormStatus } from "react-dom"

import { savePostAction, type EditorActionState } from "@/app/admin/actions"
import { MarkdownRenderer } from "@/components/markdown-renderer"
import { Button } from "@/components/ui/button"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import { Textarea } from "@/components/ui/textarea"

const initialState: EditorActionState = {}

type PostEditorProps = {
  mode: "create" | "edit"
  initialValues: {
    previousSlug?: string
    slug: string
    title: string
    kicker: string
    excerpt: string
    intro: string
    publishedAt: string
    tags: string
    content: string
  }
}

function SubmitButton({ mode }: { mode: PostEditorProps["mode"] }) {
  const { pending } = useFormStatus()

  return (
    <Button type="submit" size="lg" disabled={pending}>
      {pending ? "Saving..." : mode === "create" ? "Create post" : "Save post"}
    </Button>
  )
}

export function PostEditor({ initialValues, mode }: PostEditorProps) {
  const [state, action] = useActionState(savePostAction, initialState)
  const [content, setContent] = useState(initialValues.content)
  const [title, setTitle] = useState(initialValues.title)
  const [excerpt, setExcerpt] = useState(initialValues.excerpt)
  const [intro, setIntro] = useState(initialValues.intro)

  const previewTitle = title.trim() || "Untitled post"
  const previewDeck = useMemo(() => {
    if (intro.trim()) {
      return intro
    }

    if (excerpt.trim()) {
      return excerpt
    }

    return "Your intro or excerpt will show here as you write."
  }, [excerpt, intro])

  return (
    <form
      action={action}
      className="grid gap-6 xl:grid-cols-[minmax(0,1.1fr)_minmax(320px,0.9fr)]"
    >
      <input
        type="hidden"
        name="previousSlug"
        value={initialValues.previousSlug ?? ""}
      />
      <div className="space-y-6">
        <div className="grid gap-4 md:grid-cols-2">
          <div className="space-y-2">
            <Label htmlFor="title">Title</Label>
            <Input
              id="title"
              name="title"
              value={title}
              onChange={(event) => setTitle(event.target.value)}
              required
            />
          </div>
          <div className="space-y-2">
            <Label htmlFor="slug">Slug</Label>
            <Input id="slug" name="slug" defaultValue={initialValues.slug} />
          </div>
          <div className="space-y-2">
            <Label htmlFor="kicker">Kicker</Label>
            <Input
              id="kicker"
              name="kicker"
              defaultValue={initialValues.kicker}
              required
            />
          </div>
          <div className="space-y-2">
            <Label htmlFor="publishedAt">Published date</Label>
            <Input
              id="publishedAt"
              name="publishedAt"
              type="date"
              defaultValue={initialValues.publishedAt}
              required
            />
          </div>
        </div>

        <div className="space-y-2">
          <Label htmlFor="excerpt">Excerpt</Label>
          <Textarea
            id="excerpt"
            name="excerpt"
            value={excerpt}
            onChange={(event) => setExcerpt(event.target.value)}
            rows={3}
            required
          />
        </div>

        <div className="space-y-2">
          <Label htmlFor="intro">Intro</Label>
          <Textarea
            id="intro"
            name="intro"
            value={intro}
            onChange={(event) => setIntro(event.target.value)}
            rows={4}
            required
          />
        </div>

        <div className="space-y-2">
          <Label htmlFor="tags">Tags</Label>
          <Input
            id="tags"
            name="tags"
            defaultValue={initialValues.tags}
            placeholder="dispatches, signal, black-lantern"
          />
        </div>

        <div className="space-y-2">
          <div className="flex items-center justify-between gap-4">
            <Label htmlFor="content">Markdown body</Label>
            <p className="font-mono text-[11px] tracking-[0.18em] text-muted-foreground uppercase">
              Markdown supported
            </p>
          </div>
          <Textarea
            id="content"
            name="content"
            value={content}
            onChange={(event) => setContent(event.target.value)}
            rows={22}
            className="min-h-[28rem] font-mono text-sm"
            required
          />
        </div>

        <div className="flex flex-wrap items-center gap-3">
          <SubmitButton mode={mode} />
          <Button asChild variant="outline" size="lg">
            <Link href="/admin">Back to admin</Link>
          </Button>
          {state.error ? (
            <p className="text-sm text-destructive">{state.error}</p>
          ) : null}
        </div>
      </div>

      <div className="space-y-4">
        <div className="rounded-2xl border border-foreground/10 bg-card p-5">
          <p className="font-mono text-[11px] tracking-[0.18em] text-muted-foreground uppercase">
            Live preview
          </p>
          <div className="mt-4 space-y-3">
            <h2 className="font-heading text-3xl leading-none tracking-[-0.04em]">
              {previewTitle}
            </h2>
            <p className="text-base leading-7 text-muted-foreground">
              {previewDeck}
            </p>
          </div>
        </div>
        <div className="rounded-2xl border border-foreground/10 bg-background/80 p-5">
          <MarkdownRenderer content={content || "Start writing markdown..."} />
        </div>
      </div>
    </form>
  )
}
