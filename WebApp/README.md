# The Black Lantern Rotation WebApp

A SHADCN + Next.js site themed for The Black Lantern Rotation.

It includes:

- a lantern-themed landing page using the provided fog image
- a dispatch archive backed by markdown files in `content/blog`
- individual dispatch routes
- a broadcasts page ready for future podcast/feed links
- a local admin dashboard for writing and editing markdown content
- Railway-friendly file storage for dispatch editing with optional volume-backed persistence

## Run locally

```bash
npm install
npm run dev
```

Then open [http://localhost:3000](http://localhost:3000) or the next available
port shown by Next.js.

## Project structure

- `app/page.tsx`: Black Lantern landing page
- `app/blog/page.tsx`: dispatch archive
- `app/blog/[slug]/page.tsx`: individual dispatch pages
- `app/broadcasts/page.tsx`: broadcasts page
- `app/podcast/page.tsx`: redirect to `/broadcasts`
- `app/rotation/page.tsx`: books page
- `app/admin/*`: local content management routes
- `content/blog/*.md`: markdown dispatch files
- `public/black-lantern-fog.png`: hero/theme image asset
- `lib/blog.ts`: filesystem loader and saver for dispatches
- `lib/auth.ts`: local cookie-based admin auth
- `lib/site-content.ts`: shared brand copy and theme content

## Admin setup

Create a `.env.local` file in `WebApp` with:

```bash
SITE_ADMIN_USERNAME=admin
SITE_ADMIN_PASSWORD=change-me
SITE_SESSION_SECRET=replace-this-with-a-long-random-string
BLOG_STORAGE_DIR=
BLOG_SEED_DIR=
```

Then open `/admin`.

## Railway deploy notes

The admin editor is prepared for Railway-style persistent storage.

How it behaves:

- If no volume is attached, editable dispatches live in `content/blog`.
- If Railway attaches a volume, the app automatically uses `RAILWAY_VOLUME_MOUNT_PATH/blog` as the editable storage directory.
- If `BLOG_STORAGE_DIR` is set, that path wins.
- If the editable storage directory is empty on first boot, the app copies the checked-in markdown posts from `content/blog` into it automatically.

Recommended Railway setup:

1. Deploy the `WebApp` service.
2. Attach a volume to the service and mount it at `/app/data`.
3. Set:
   - `SITE_ADMIN_USERNAME`
   - `SITE_ADMIN_PASSWORD`
   - `SITE_SESSION_SECRET`
4. Optionally set `BLOG_STORAGE_DIR=/app/data/blog` if you want the path to be explicit, though the app will infer that path automatically when Railway provides `RAILWAY_VOLUME_MOUNT_PATH`.

After deploy:

- Open `/admin` and sign in.
- The admin dashboard will show the active storage path so you can confirm the app is writing to the mounted volume instead of the packaged app directory.

Deploy helpers:

```bash
npm run railway:check
npm run railway:up
```

Optional flags:

- `npm run railway:up -- --skip-build` skips the local pre-deploy build check.
- `npm run railway:up -- --ci` forwards Railway CLI flags such as `--ci`, `--detach`, or `--service`.

Default deploy target:

- project: `awake-adaptation`
- environment: `production`
- service: `secure-appreciation`

You can override those defaults with:

- `RAILWAY_DEPLOY_PROJECT`
- `RAILWAY_DEPLOY_ENVIRONMENT`
- `RAILWAY_DEPLOY_SERVICE`
