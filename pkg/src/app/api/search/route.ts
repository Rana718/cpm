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

async function fetchLatestTag(
  owner: string,
  repo: string,
  headers: HeadersInit
): Promise<string | null> {
  try {
    const res = await fetch(
      `https://api.github.com/repos/${owner}/${repo}/tags?per_page=1`,
      { headers, next: { revalidate: 3600 } }
    );
    // Don't fail the whole request on rate limit — just skip version
    if (res.status === 403 || res.status === 429 || !res.ok) return null;
    const tags: GitHubTag[] = await res.json();
    return tags.length > 0 ? tags[0].name : null;
  } catch {
    return null;
  }
}

async function searchGitHub(
  query: string,
  language: "C++" | "C" | null,
  headers: HeadersInit
): Promise<{ repos: GitHubRepo[]; rateLimited: boolean }> {
  const langFilter = language ? ` language:${language}` : "";
  const q = encodeURIComponent(`${query}${langFilter} stars:>5`);
  const res = await fetch(
    `https://api.github.com/search/repositories?q=${q}&sort=stars&order=desc&per_page=15`,
    { headers, next: { revalidate: 60 } }
  );
  if (res.status === 403 || res.status === 429) {
    return { repos: [], rateLimited: true };
  }
  if (!res.ok) return { repos: [], rateLimited: false };
  const data = await res.json();
  return { repos: data.items ?? [], rateLimited: false };
}

// Languages that are clearly NOT C/C++ libraries
const NON_CPP_LANGUAGES = new Set([
  "javascript", "python", "ruby", "java", "go", "rust",
  "kotlin", "swift", "php", "perl", "scala", "haskell",
  "elixir", "erlang", "dart", "lua", "r", "julia",
  "csharp", "c#", "f#", "ocaml", "nim", "zig",
]);

// Repo names that are clearly bindings/wrappers for other languages
const NAME_EXCLUDE = [
  "binding", "bindings", "wrapper", "wrappers", "napi",
  "-node", "node-", "-py", "-python", "python-",
  "-rb", "-ruby", "-java", "-golang", "-dotnet", "-swift", "-php",
];

// Well-known repos that are applications/databases, not usable C/C++ libraries
const APP_REPOS = new Set([
  "redis/redis", "antirez/redis",
  "curl/curl",
  "torvalds/linux",
  "git/git",
  "nginx/nginx",
  "postgres/postgres", "postgresql/postgresql",
  "sqlite/sqlite",
  "mysql/mysql-server",
  "memcached/memcached",
  "apache/httpd",
  "openssl/openssl",
  "ffmpeg/ffmpeg",
  "videolan/vlc",
  "obsproject/obs-studio",
  "php/php-src",
  "python/cpython",
  "ruby/ruby",
]);

function isValidCppRepo(repo: GitHubRepo): boolean {
  const lang = (repo.language ?? "").toLowerCase();

  // If language is explicitly a non-C language, exclude it
  if (NON_CPP_LANGUAGES.has(lang)) return false;

  // Exclude well-known applications (not libraries)
  if (APP_REPOS.has(repo.full_name.toLowerCase())) return false;

  // Exclude clear binding/wrapper repos by name
  const name = repo.name.toLowerCase();
  if (NAME_EXCLUDE.some((term) => name.includes(term))) return false;

  return true;
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

  // Run 3 parallel searches: C++, C, and no language filter (catches libs like llhttp)
  const [cppResult, cResult, generalResult] = await Promise.all([
    searchGitHub(query, "C++", headers),
    searchGitHub(query, "C", headers),
    searchGitHub(query, null, headers),
  ]);

  // Check if rate limited
  if (cppResult.rateLimited || cResult.rateLimited || generalResult.rateLimited) {
    return NextResponse.json(
      { error: "GitHub API rate limit exceeded. Add GITHUB_TOKEN to .env.local or wait." },
      { status: 429 }
    );
  }

  // Merge, deduplicate by id, filter non-C/C++ repos, sort by stars
  const seen = new Set<number>();
  const merged: GitHubRepo[] = [];
  for (const repo of [...cppResult.repos, ...cResult.repos, ...generalResult.repos]) {
    if (!seen.has(repo.id) && isValidCppRepo(repo)) {
      seen.add(repo.id);
      merged.push(repo);
    }
  }
  merged.sort((a, b) => b.stargazers_count - a.stargazers_count);

  // Fetch latest tags in parallel
  const results: PackageResult[] = await Promise.all(
    merged.map(async (repo) => {
      const latestVersion = await fetchLatestTag(repo.owner.login, repo.name, headers);
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

  return NextResponse.json({ results, total: results.length });
}
