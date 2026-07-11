import { NextRequest, NextResponse } from "next/server";

export interface PackageDetail {
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
  defaultBranch: string;
  htmlUrl: string;
  readme: string | null;
  tags: { name: string; date: string | null }[];
  latestVersion: string | null;
  cpmInstall: string;
  cpmToml: string;
  cpmSystemToml: string;
}

export async function GET(req: NextRequest) {
  const { searchParams } = new URL(req.url);
  const owner = searchParams.get("owner");
  const repo = searchParams.get("repo");

  if (!owner || !repo) {
    return NextResponse.json({ error: "Missing owner or repo" }, { status: 400 });
  }

  const token = process.env.GITHUB_TOKEN;
  const headers: HeadersInit = { Accept: "application/vnd.github+json" };
  if (token) headers["Authorization"] = `Bearer ${token}`;

  const [repoRes, tagsRes, readmeRes] = await Promise.all([
    fetch(`https://api.github.com/repos/${owner}/${repo}`, { headers, next: { revalidate: 3600 } }),
    fetch(`https://api.github.com/repos/${owner}/${repo}/tags?per_page=20`, { headers, next: { revalidate: 3600 } }),
    fetch(`https://api.github.com/repos/${owner}/${repo}/readme`, {
      headers: { ...headers, Accept: "application/vnd.github.raw" },
      next: { revalidate: 3600 },
    }),
  ]);

  if (!repoRes.ok) {
    return NextResponse.json({ error: "Repository not found" }, { status: 404 });
  }

  const repoData = await repoRes.json();
  const tagsData = tagsRes.ok ? await tagsRes.json() : [];
  const readmeText = readmeRes.ok ? await readmeRes.text() : null;

  // Fetch tag dates
  const tagsWithDates = await Promise.all(
    tagsData.slice(0, 20).map(async (tag: { name: string; commit: { url: string } }) => {
      try {
        const commitRes = await fetch(tag.commit.url, { headers, next: { revalidate: 3600 } });
        if (!commitRes.ok) return { name: tag.name, date: null };
        const commitData = await commitRes.json();
        return { name: tag.name, date: commitData.commit?.committer?.date ?? null };
      } catch {
        return { name: tag.name, date: null };
      }
    })
  );

  const latestVersion = tagsWithDates.length > 0 ? tagsWithDates[0].name : null;
  const versionSuffix = latestVersion ? `@${latestVersion}` : "";

  const detail: PackageDetail = {
    name: repoData.name,
    owner: repoData.owner.login,
    ownerAvatar: repoData.owner.avatar_url,
    fullName: repoData.full_name,
    description: repoData.description ?? "No description available.",
    stars: repoData.stargazers_count,
    forks: repoData.forks_count,
    language: repoData.language,
    topics: repoData.topics ?? [],
    pushedAt: repoData.pushed_at,
    license: repoData.license?.spdx_id ?? null,
    defaultBranch: repoData.default_branch,
    htmlUrl: repoData.html_url,
    readme: readmeText,
    tags: tagsWithDates,
    latestVersion,
    cpmInstall: `cpm add github:${repoData.full_name}${versionSuffix}`,
    cpmToml: latestVersion
      ? `${repoData.name} = "github:${repoData.full_name}@${latestVersion}"`
      : `${repoData.name} = "github:${repoData.full_name}"`,
    cpmSystemToml: latestVersion
      ? `${repoData.name} = "github:${repoData.full_name}@${latestVersion}"`
      : `${repoData.name} = "github:${repoData.full_name}"`,
  };

  return NextResponse.json(detail);
}
