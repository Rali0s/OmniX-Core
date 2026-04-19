import fs from "node:fs/promises"
import path from "node:path"

import matter from "gray-matter"

export type BlogPost = {
  slug: string
  title: string
  kicker: string
  excerpt: string
  intro: string
  publishedAt: string
  publishedLabel: string
  readingTime: string
  tags: string[]
  content: string
}

export type BlogPostInput = {
  previousSlug?: string
  slug?: string
  title: string
  kicker: string
  excerpt: string
  intro: string
  publishedAt: string
  tags: string[]
  content: string
}

export type BlogStorageDetails = {
  storageDir: string
  seedDir: string
  mountPath: string | null
  isUsingPersistentStorage: boolean
}

const DEFAULT_BLOG_DIR = path.join(process.cwd(), "content", "blog")

function resolveDirectoryPath(value: string) {
  return path.isAbsolute(value) ? value : path.resolve(process.cwd(), value)
}

function getBlogStorageDetails(): BlogStorageDetails {
  const mountPath = process.env.RAILWAY_VOLUME_MOUNT_PATH?.trim() || null
  const configuredStorageDir = process.env.BLOG_STORAGE_DIR?.trim()
  const configuredSeedDir = process.env.BLOG_SEED_DIR?.trim()
  const storageDir = resolveDirectoryPath(
    configuredStorageDir || (mountPath ? path.join(mountPath, "blog") : DEFAULT_BLOG_DIR)
  )
  const seedDir = resolveDirectoryPath(configuredSeedDir || DEFAULT_BLOG_DIR)

  return {
    storageDir,
    seedDir,
    mountPath,
    isUsingPersistentStorage: storageDir !== DEFAULT_BLOG_DIR,
  }
}

function estimateReadingTime(content: string) {
  const wordCount = content.trim().split(/\s+/).filter(Boolean).length
  const minutes = Math.max(1, Math.ceil(wordCount / 200))

  return `${minutes} min read`
}

function normalizePublishedAt(value: unknown) {
  if (value instanceof Date) {
    return value.toISOString().slice(0, 10)
  }

  if (typeof value === "string") {
    const trimmed = value.trim()

    if (/^\d{4}-\d{2}-\d{2}$/.test(trimmed)) {
      return trimmed
    }

    const parsed = new Date(trimmed)

    if (!Number.isNaN(parsed.getTime())) {
      return parsed.toISOString().slice(0, 10)
    }

    return trimmed
  }

  return ""
}

function formatPublishedDate(value: string) {
  const parsed = new Date(value)

  if (Number.isNaN(parsed.getTime())) {
    return value
  }

  return new Intl.DateTimeFormat("en-US", {
    month: "long",
    day: "numeric",
    year: "numeric",
  }).format(parsed)
}

function normalizeTags(tags: unknown) {
  if (Array.isArray(tags)) {
    return tags.map((tag) => String(tag).trim()).filter(Boolean)
  }

  if (typeof tags === "string") {
    return tags
      .split(",")
      .map((tag) => tag.trim())
      .filter(Boolean)
  }

  return []
}

function sortPosts(posts: BlogPost[]) {
  return [...posts].sort((left, right) => {
    const leftTime = Date.parse(left.publishedAt)
    const rightTime = Date.parse(right.publishedAt)

    if (Number.isNaN(leftTime) || Number.isNaN(rightTime)) {
      return right.slug.localeCompare(left.slug)
    }

    return rightTime - leftTime
  })
}

function slugify(input: string) {
  return input
    .toLowerCase()
    .trim()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 80)
}

async function ensureBlogDirectory() {
  const { storageDir, seedDir } = getBlogStorageDetails()

  await fs.mkdir(storageDir, { recursive: true })

  const storageFiles = await fs.readdir(storageDir)

  if (storageFiles.some((fileName) => fileName.endsWith(".md"))) {
    return
  }

  if (storageDir === seedDir) {
    return
  }

  try {
    const seedFiles = await fs.readdir(seedDir)

    await Promise.all(
      seedFiles
        .filter((fileName) => fileName.endsWith(".md"))
        .map((fileName) =>
          fs.copyFile(
            path.join(seedDir, fileName),
            path.join(storageDir, fileName)
          )
        )
    )
  } catch (error) {
    const missingSeedDirectory =
      error instanceof Error &&
      "code" in error &&
      error.code === "ENOENT"

    if (!missingSeedDirectory) {
      throw error
    }
  }
}

async function readBlogFile(fileName: string) {
  const { storageDir } = getBlogStorageDetails()
  const fullPath = path.join(storageDir, fileName)
  const source = await fs.readFile(fullPath, "utf8")
  const parsed = matter(source)
  const publishedAt = normalizePublishedAt(parsed.data.publishedAt)
  const slug =
    typeof parsed.data.slug === "string" && parsed.data.slug.trim()
      ? parsed.data.slug.trim()
      : fileName.replace(/\.md$/, "")

  return {
    slug,
    title: String(parsed.data.title ?? slug),
    kicker: String(parsed.data.kicker ?? "Post"),
    excerpt: String(parsed.data.excerpt ?? ""),
    intro: String(parsed.data.intro ?? ""),
    publishedAt,
    publishedLabel: formatPublishedDate(publishedAt),
    readingTime: estimateReadingTime(parsed.content),
    tags: normalizeTags(parsed.data.tags),
    content: parsed.content.trim(),
  } satisfies BlogPost
}

export async function getAllPosts() {
  await ensureBlogDirectory()
  const { storageDir } = getBlogStorageDetails()

  const files = await fs.readdir(storageDir)
  const posts = await Promise.all(
    files
      .filter((fileName) => fileName.endsWith(".md"))
      .map((fileName) => readBlogFile(fileName))
  )

  return sortPosts(posts)
}

export async function getPostBySlug(slug: string) {
  try {
    return await readBlogFile(`${slug}.md`)
  } catch {
    return null
  }
}

export async function savePost(input: BlogPostInput) {
  await ensureBlogDirectory()
  const { storageDir } = getBlogStorageDetails()

  const nextSlug = slugify(input.slug?.trim() || input.title)

  if (!nextSlug) {
    throw new Error("A title or slug is required.")
  }

  const frontmatter = {
    slug: nextSlug,
    title: input.title.trim(),
    kicker: input.kicker.trim() || "Post",
    excerpt: input.excerpt.trim(),
    intro: input.intro.trim(),
    publishedAt: input.publishedAt.trim(),
    tags: input.tags,
  }

  const nextPath = path.join(storageDir, `${nextSlug}.md`)
  const previousSlug = input.previousSlug?.trim()

  if (previousSlug && previousSlug !== nextSlug) {
    const previousPath = path.join(storageDir, `${previousSlug}.md`)
    await fs.rm(previousPath, { force: true })
  }

  const markdown = matter.stringify(`${input.content.trim()}\n`, frontmatter)
  await fs.writeFile(nextPath, markdown, "utf8")

  return nextSlug
}

export async function deletePost(slug: string) {
  await ensureBlogDirectory()
  const { storageDir } = getBlogStorageDetails()
  await fs.rm(path.join(storageDir, `${slug}.md`), { force: true })
}

export async function getBlogStorageInfo() {
  await ensureBlogDirectory()

  return getBlogStorageDetails()
}
