"use server"

import { revalidatePath } from "next/cache"
import { redirect } from "next/navigation"

import {
  clearAdminSession,
  createAdminSession,
  getAdminDefaults,
  requireAdmin,
  validateAdminCredentials,
} from "@/lib/auth"
import { deletePost, savePost } from "@/lib/blog"

export type LoginActionState = {
  error?: string
}

export type EditorActionState = {
  error?: string
}

export async function loginAction(
  _previousState: LoginActionState,
  formData: FormData
) {
  const username = String(formData.get("username") ?? "")
  const password = String(formData.get("password") ?? "")
  const nextPath = String(formData.get("next") ?? "/admin")
  const admin = getAdminDefaults()

  if (admin.isProductionLocked) {
    return {
      error:
        "Admin login is locked in production until SITE_ADMIN_USERNAME, SITE_ADMIN_PASSWORD, and SITE_SESSION_SECRET are set.",
    }
  }

  if (!validateAdminCredentials(username, password)) {
    return {
      error: "Those admin credentials did not match.",
    }
  }

  await createAdminSession(username.trim())
  redirect(nextPath)
}

export async function logoutAction() {
  await clearAdminSession()
  redirect("/admin/login")
}

export async function savePostAction(
  _previousState: EditorActionState,
  formData: FormData
) {
  await requireAdmin("/admin")

  const title = String(formData.get("title") ?? "").trim()
  const content = String(formData.get("content") ?? "").trim()

  if (!title) {
    return { error: "Title is required." }
  }

  if (!content) {
    return { error: "Markdown content is required." }
  }

  const previousSlug = String(formData.get("previousSlug") ?? "").trim()
  const slug = await savePost({
    previousSlug: previousSlug || undefined,
    slug: String(formData.get("slug") ?? ""),
    title,
    kicker: String(formData.get("kicker") ?? ""),
    excerpt: String(formData.get("excerpt") ?? ""),
    intro: String(formData.get("intro") ?? ""),
    publishedAt: String(formData.get("publishedAt") ?? ""),
    tags: String(formData.get("tags") ?? "")
      .split(",")
      .map((tag) => tag.trim())
      .filter(Boolean),
    content,
  })

  revalidatePath("/")
  revalidatePath("/blog")
  revalidatePath("/admin")
  revalidatePath("/admin/posts/new")
  revalidatePath(`/admin/posts/${slug}/edit`)
  revalidatePath(`/blog/${slug}`)

  if (previousSlug && previousSlug !== slug) {
    revalidatePath(`/blog/${previousSlug}`)
  }

  redirect(`/admin/posts/${slug}/edit?saved=1`)
}

export async function deletePostAction(formData: FormData) {
  await requireAdmin("/admin")

  const slug = String(formData.get("slug") ?? "").trim()

  if (!slug) {
    redirect("/admin")
  }

  await deletePost(slug)

  revalidatePath("/")
  revalidatePath("/blog")
  revalidatePath("/admin")
  revalidatePath(`/blog/${slug}`)

  redirect("/admin?deleted=1")
}
