"use client";

import { useState, useEffect } from "react";
import { useParams, useRouter } from "next/navigation";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import {
  AiOutlineStar,
  AiOutlineCopy,
  AiOutlineCheck,
  AiOutlineTag,
  AiOutlineClockCircle,
  AiOutlineArrowLeft,
} from "react-icons/ai";
import { BiGitRepoForked } from "react-icons/bi";
import { FaGithub } from "react-icons/fa";
import type { PackageDetail } from "@/app/api/package/route";

type Tab = "readme" | "versions" | "install";

function timeAgo(dateStr: string): string {
  const diff = Date.now() - new Date(dateStr).getTime();
  const days = Math.floor(diff / 86400000);
  if (days < 1) return "today";
  if (days < 7) return `${days}d ago`;
  if (days < 30) return `${Math.floor(days / 7)}w ago`;
  if (days < 365) return `${Math.floor(days / 30)}mo ago`;
  return `${Math.floor(days / 365)}y ago`;
}

function CopyButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);
  const handleCopy = async () => {
    await navigator.clipboard.writeText(text);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };
  return (
    <button
      onClick={handleCopy}
      className="p-1.5 rounded-md text-muted-foreground hover:text-foreground hover:bg-muted transition-colors flex-shrink-0"
    >
      {copied ? (
        <AiOutlineCheck className="h-4 w-4 text-green-500" />
      ) : (
        <AiOutlineCopy className="h-4 w-4" />
      )}
    </button>
  );
}

function CodeBlock({ label, code, hint }: { label: string; code: string; hint?: string }) {
  return (
    <div className="flex flex-col gap-1.5">
      <div className="flex items-center justify-between">
        <span className="text-xs font-semibold text-muted-foreground uppercase tracking-wider">
          {label}
        </span>
        {hint && <span className="text-xs text-muted-foreground">{hint}</span>}
      </div>
      <div className="flex items-center gap-2 bg-muted rounded-lg px-4 py-3 font-mono text-sm border border-border">
        <code className="flex-1 text-foreground overflow-x-auto whitespace-nowrap">{code}</code>
        <CopyButton text={code} />
      </div>
    </div>
  );
}

export default function PackagePage() {
  const params = useParams();
  const router = useRouter();
  const owner = params.owner as string;
  const repo = params.repo as string;

  const [pkg, setPkg] = useState<PackageDetail | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [tab, setTab] = useState<Tab>("readme");

  useEffect(() => {
    async function load() {
      try {
        const res = await fetch(`/api/package?owner=${owner}&repo=${repo}`);
        const data = await res.json();
        if (!res.ok) setError(data.error ?? "Failed to load package");
        else setPkg(data);
      } catch {
        setError("Network error.");
      } finally {
        setLoading(false);
      }
    }
    load();
  }, [owner, repo]);

  if (loading) {
    return (
      <div className="min-h-screen flex flex-col">
        <Header />
        <div className="flex-1 max-w-6xl w-full mx-auto px-4 py-10 space-y-4">
          <div className="h-8 w-48 bg-muted rounded animate-pulse" />
          <div className="h-32 bg-muted rounded-xl animate-pulse" />
          <div className="h-96 bg-muted rounded-xl animate-pulse" />
        </div>
      </div>
    );
  }

  if (error || !pkg) {
    return (
      <div className="min-h-screen flex flex-col">
        <Header />
        <div className="flex-1 flex items-center justify-center text-muted-foreground">
          <div className="text-center">
            <p className="text-lg font-medium">Package not found</p>
            <p className="text-sm mt-1">{error}</p>
            <button
              onClick={() => router.back()}
              className="mt-4 text-sm text-primary hover:underline"
            >
              ← Go back
            </button>
          </div>
        </div>
      </div>
    );
  }

  const tabs: { id: Tab; label: string }[] = [
    { id: "readme", label: "README" },
    { id: "versions", label: `Versions (${pkg.tags.length})` },
    { id: "install", label: "Install" },
  ];

  return (
    <div className="min-h-screen flex flex-col">
      <Header />

      <main className="flex-1 w-full max-w-6xl mx-auto px-4 py-8">
        {/* Back */}
        <button
          onClick={() => router.back()}
          className="flex items-center gap-1.5 text-sm text-muted-foreground hover:text-foreground transition-colors mb-6"
        >
          <AiOutlineArrowLeft className="h-4 w-4" />
          Back to search
        </button>

        <div className="flex flex-col lg:flex-row gap-8">
          {/* Main content */}
          <div className="flex-1 min-w-0">
            {/* Package header */}
            <div className="flex flex-col gap-4 mb-6 p-5 rounded-xl border border-border bg-card">
              <div className="flex items-start justify-between gap-4 flex-wrap">
                <div className="flex items-center gap-3">
                  <img
                    src={pkg.ownerAvatar}
                    alt={pkg.owner}
                    className="h-10 w-10 rounded-full border border-border"
                  />
                  <div>
                    <div className="flex items-center gap-1.5 flex-wrap">
                      <span className="text-muted-foreground">{pkg.owner}</span>
                      <span className="text-muted-foreground">/</span>
                      <span className="text-xl font-bold text-foreground">{pkg.name}</span>
                      {pkg.latestVersion && (
                        <span className="flex items-center gap-1 text-xs bg-primary/10 text-primary border border-primary/20 rounded-full px-2 py-0.5 font-mono">
                          <AiOutlineTag className="h-3 w-3" />
                          {pkg.latestVersion}
                        </span>
                      )}
                    </div>
                    <p className="text-sm text-muted-foreground mt-1">{pkg.description}</p>
                  </div>
                </div>
                <a
                  href={pkg.htmlUrl}
                  target="_blank"
                  rel="noopener noreferrer"
                  className="flex items-center gap-2 text-sm px-3 py-1.5 rounded-lg border border-border hover:bg-muted transition-colors text-foreground"
                >
                  <FaGithub className="h-4 w-4" />
                  GitHub
                </a>
              </div>

              {/* Meta */}
              <div className="flex flex-wrap gap-4 text-sm text-muted-foreground border-t border-border pt-4">
                <span className="flex items-center gap-1.5">
                  <AiOutlineStar className="h-4 w-4" />
                  {pkg.stars.toLocaleString()} stars
                </span>
                <span className="flex items-center gap-1.5">
                  <BiGitRepoForked className="h-4 w-4" />
                  {pkg.forks.toLocaleString()} forks
                </span>
                {pkg.language && (
                  <span className="flex items-center gap-1.5">
                    <span className="h-3 w-3 rounded-full bg-primary inline-block" />
                    {pkg.language}
                  </span>
                )}
                {pkg.license && <span>{pkg.license}</span>}
                <span className="flex items-center gap-1.5">
                  <AiOutlineClockCircle className="h-4 w-4" />
                  Updated {timeAgo(pkg.pushedAt)}
                </span>
              </div>

              {/* Topics */}
              {pkg.topics.length > 0 && (
                <div className="flex flex-wrap gap-2">
                  {pkg.topics.map((t) => (
                    <span
                      key={t}
                      className="text-xs bg-secondary text-secondary-foreground rounded-full px-2.5 py-0.5"
                    >
                      {t}
                    </span>
                  ))}
                </div>
              )}
            </div>

            {/* Tabs */}
            <div className="flex gap-1 border-b border-border mb-6">
              {tabs.map((t) => (
                <button
                  key={t.id}
                  onClick={() => setTab(t.id)}
                  className={`px-4 py-2.5 text-sm font-medium transition-colors border-b-2 -mb-px ${
                    tab === t.id
                      ? "border-primary text-foreground"
                      : "border-transparent text-muted-foreground hover:text-foreground"
                  }`}
                >
                  {t.label}
                </button>
              ))}
            </div>

            {/* Tab: README */}
            {tab === "readme" && (
              <div className="prose prose-sm dark:prose-invert max-w-none rounded-xl border border-border bg-card p-6 overflow-x-auto">
                {pkg.readme ? (
                  <ReactMarkdown remarkPlugins={[remarkGfm]}>{pkg.readme}</ReactMarkdown>
                ) : (
                  <p className="text-muted-foreground">No README available for this package.</p>
                )}
              </div>
            )}

            {/* Tab: VERSIONS */}
            {tab === "versions" && (
              <div className="rounded-xl border border-border bg-card overflow-hidden">
                {pkg.tags.length === 0 ? (
                  <p className="p-6 text-muted-foreground text-sm">No tagged versions found.</p>
                ) : (
                  <table className="w-full text-sm">
                    <thead>
                      <tr className="border-b border-border bg-muted/50">
                        <th className="text-left px-4 py-3 font-medium text-muted-foreground">Version</th>
                        <th className="text-left px-4 py-3 font-medium text-muted-foreground">Released</th>
                        <th className="text-left px-4 py-3 font-medium text-muted-foreground">cpm add</th>
                      </tr>
                    </thead>
                    <tbody>
                      {pkg.tags.map((tag, i) => (
                        <tr key={tag.name} className="border-b border-border last:border-0 hover:bg-muted/30 transition-colors">
                          <td className="px-4 py-3">
                            <div className="flex items-center gap-2">
                              <span className="font-mono font-medium text-foreground">{tag.name}</span>
                              {i === 0 && (
                                <span className="text-xs bg-green-500/10 text-green-600 dark:text-green-400 border border-green-500/20 rounded-full px-2 py-0.5">
                                  latest
                                </span>
                              )}
                            </div>
                          </td>
                          <td className="px-4 py-3 text-muted-foreground">
                            {tag.date ? timeAgo(tag.date) : "—"}
                          </td>
                          <td className="px-4 py-3">
                            <div className="flex items-center gap-2 bg-muted rounded-md px-3 py-1.5 font-mono text-xs w-fit max-w-xs">
                              <code className="overflow-x-auto whitespace-nowrap text-foreground">
                                cpm add github:{pkg.fullName}@{tag.name}
                              </code>
                              <CopyButton text={`cpm add github:${pkg.fullName}@${tag.name}`} />
                            </div>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                )}
              </div>
            )}

            {/* Tab: INSTALL */}
            {tab === "install" && (
              <div className="flex flex-col gap-6">
                {/* CLI */}
                <div className="rounded-xl border border-border bg-card p-5 flex flex-col gap-4">
                  <h3 className="font-semibold text-foreground">Terminal</h3>
                  <CodeBlock label="Install command" code={pkg.cpmInstall} />
                </div>

                {/* cpm.toml – header-only */}
                <div className="rounded-xl border border-border bg-card p-5 flex flex-col gap-4">
                  <div>
                    <h3 className="font-semibold text-foreground">Header-only dependency</h3>
                    <p className="text-sm text-muted-foreground mt-1">
                      Use this for header-only libraries (e.g. nlohmann/json, fmtlib). Add to your{" "}
                      <code className="bg-muted px-1 rounded text-xs">cpm.toml</code>:
                    </p>
                  </div>
                  <CodeBlock
                    label="cpm.toml → [dependencies]"
                    code={pkg.cpmToml}
                    hint="header-only"
                  />
                  <pre className="bg-muted rounded-lg p-4 text-xs font-mono text-muted-foreground overflow-x-auto">{`[project]
name = "myapp"
version = "0.1.0"

[dependencies]
${pkg.cpmToml}`}</pre>
                </div>

                {/* cpm.toml – system/compiled */}
                <div className="rounded-xl border border-border bg-card p-5 flex flex-col gap-4">
                  <div>
                    <h3 className="font-semibold text-foreground">Compiled / system dependency</h3>
                    <p className="text-sm text-muted-foreground mt-1">
                      Use this for compiled libraries that need to be built (e.g. hiredis, seastar, grpc). Add to your{" "}
                      <code className="bg-muted px-1 rounded text-xs">cpm.toml</code>:
                    </p>
                  </div>
                  <CodeBlock
                    label="cpm.toml → [system-dependencies]"
                    code={pkg.cpmSystemToml}
                    hint="compiled"
                  />
                  <pre className="bg-muted rounded-lg p-4 text-xs font-mono text-muted-foreground overflow-x-auto">{`[project]
name = "myapp"
version = "0.1.0"

[system-dependencies]
${pkg.cpmSystemToml}`}</pre>
                </div>
              </div>
            )}
          </div>

          {/* Sidebar — quick install */}
          <aside className="w-full lg:w-80 flex-shrink-0 flex flex-col gap-4">
            <div className="rounded-xl border border-border bg-card p-5 flex flex-col gap-4 sticky top-20">
              <h3 className="font-semibold text-foreground text-sm">Quick Install</h3>
              <CodeBlock label="cpm add" code={pkg.cpmInstall} />
              <CodeBlock label="cpm.toml [dependencies]" code={pkg.cpmToml} />
              <CodeBlock label="cpm.toml [system-dependencies]" code={pkg.cpmSystemToml} />
              <a
                href={pkg.htmlUrl}
                target="_blank"
                rel="noopener noreferrer"
                className="flex items-center justify-center gap-2 w-full py-2 rounded-lg border border-border text-sm hover:bg-muted transition-colors text-foreground"
              >
                <FaGithub className="h-4 w-4" />
                View on GitHub
              </a>
            </div>
          </aside>
        </div>
      </main>

      <footer className="border-t border-border py-6 text-center text-xs text-muted-foreground">
        cpm registry — powered by GitHub API
      </footer>
    </div>
  );
}

function Header() {
  const router = useRouter();
  return (
    <header className="border-b border-border bg-card/80 backdrop-blur sticky top-0 z-10">
      <div className="max-w-6xl mx-auto px-4 h-14 flex items-center justify-between">
        <button
          onClick={() => router.push("/")}
          className="flex items-center gap-2 font-semibold text-foreground hover:text-primary transition-colors"
        >
          <AiOutlineTag className="h-5 w-5 text-primary" />
          cpm registry
          <span className="text-xs bg-secondary text-secondary-foreground rounded-full px-2 py-0.5 font-mono">
            beta
          </span>
        </button>
        <a
          href="https://github.com/Rana718/cpm"
          target="_blank"
          rel="noopener noreferrer"
          className="flex items-center gap-1.5 text-sm text-muted-foreground hover:text-foreground transition-colors"
        >
          <FaGithub className="h-4 w-4" />
          <span className="hidden sm:inline">GitHub</span>
        </a>
      </div>
    </header>
  );
}
