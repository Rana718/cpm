import { NextRequest, NextResponse } from "next/server";

export interface GitHubRepo {
  id: number;
  full_name: string;
  name: string;
  owner: { login: string; avatar_url: string };
  description: string | null;
  html_url: string;
  stargazers_count: number;
  forks_count: number;
  language: string | null;
  topics: string[];
  pushed_at: string;
  license: { spdx_id: string } | null;
  default_branch: string;
}

export interface GitHubTag {
  name: string;
  commit: { sha: string };
}

export interface PackageResult {
  id: number;
  name: string;
  owner: string;
  ownerAvatar: string;
  fullName: string;
  description: string;
  stars: number;
  forks: number;
  language: string | null;
  topics: string[];
  pushedAt: string;
  license: string | null;
  latestVersion: string | null;
  cpmInstall: string;
  cpmToml: string;
}

async function fetchLatestTag(owner: string, repo: string, token?: string): Promise<string | null> {
  try {
    const headers: HeadersInit = { Accept: "application/vnd.github+json" };
    if (token) headers["Authorization"] = `Bearer ${token}`;

    const res = await fetch(
      `https://api.github.com/repos/${owner}/${repo}/tags?per_page=1`,
      { headers, next: { revalidate: 3600 } }
    );
    if (!res.ok) return null;
    const tags: GitHubTag[] = await res.json();
    return tags.length > 0 ? tags[0].name : null;
  } catch {
    return null;
  }
}

export async function GET(req: NextRequest) {
  const { searchParams } = new URL(req.url);
  const query = searchParams.get("q")?.trim();

  if (!query) {
    return NextResponse.json({ error: "Missing query parameter" }, { status: 400 });
  }

  const token = process.env.GITHUB_TOKEN;
  const headers: HeadersInit = { Accept: "application/vnd.github+json" };
  if (token) headers["Authorization"] = `Bearer ${token}`;

  // Search GitHub for C/C++ repos matching the query
  const searchQuery = encodeURIComponent(
    `${query} language:C++ language:C stars:>5`
  );

  const res = await fetch(
    `https://api.github.com/search/repositories?q=${searchQuery}&sort=stars&order=desc&per_page=12`,
    { headers, next: { revalidate: 60 } }
  );

  if (!res.ok) {
    const err = await res.json();
    return NextResponse.json(
      { error: err.message ?? "GitHub API error" },
      { status: res.status }
    );
  }

  const data = await res.json();
  const repos: GitHubRepo[] = data.items ?? [];

  // Fetch latest tags in parallel (limit to avoid rate limiting)
  const results: PackageResult[] = await Promise.all(
    repos.map(async (repo) => {
      const latestVersion = await fetchLatestTag(repo.owner.login, repo.name, token);
      const versionSuffix = latestVersion ? `@${latestVersion}` : "";
      const cpmInstall = `cpm add github:${repo.full_name}${versionSuffix}`;
      const cpmToml = latestVersion
        ? `${repo.name} = "github:${repo.full_name}@${latestVersion}"`
        : `${repo.name} = "github:${repo.full_name}"`;

      return {
        id: repo.id,
        name: repo.name,
        owner: repo.owner.login,
        ownerAvatar: repo.owner.avatar_url,
        fullName: repo.full_name,
        description: repo.description ?? "No description available.",
        stars: repo.stargazers_count,
        forks: repo.forks_count,
        language: repo.language,
        topics: repo.topics ?? [],
        pushedAt: repo.pushed_at,
        license: repo.license?.spdx_id ?? null,
        latestVersion,
        cpmInstall,
        cpmToml,
      };
    })
  );

  return NextResponse.json({ results, total: data.total_count });
}
