# cpm registry

Package search and discovery for the [cpm](https://github.com/Rana718/cpm) C/C++ package manager.

## Setup

1. **Install dependencies:**
   ```bash
   bun install
   ```

2. **Create `.env.local` with your GitHub token** (required):
   ```bash
   GITHUB_TOKEN=ghp_xxxxxxxxxxxxx
   ```
   - Get a token at https://github.com/settings/tokens
   - No scopes needed for public repos
   - Without this, you'll hit rate limits (60 requests/hour)
   - With token: 5000 requests/hour

3. **Run dev server:**
   ```bash
   bun run dev
   ```

4. **Build:**
   ```bash
   bun run build
   bun run start
   ```

## Features

- Search C/C++ libraries from GitHub
- Click package → see README, versions, install instructions
- Copy-ready `cpm add` and `cpm.toml` snippets
- Separates header-only vs compiled dependencies
- Filters out language bindings/wrappers

## Tech Stack

- Next.js 16 (App Router)
- React 19
- Tailwind CSS 4
- shadcn/ui
- react-icons
- react-markdown (for README rendering)
- GitHub REST API
