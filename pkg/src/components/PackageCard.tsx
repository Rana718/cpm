"use client";

import { useState } from "react";
import {
  AiOutlineStar,
  AiOutlineCopy,
  AiOutlineCheck,
  AiOutlineLink,
  AiOutlineTag,
  AiOutlineClockCircle,
} from "react-icons/ai";
import { BiGitRepoForked } from "react-icons/bi";
import type { PackageResult } from "@/app/api/search/route";

function timeAgo(dateStr: string): string {
  const diff = Date.now() - new Date(dateStr).getTime();
  const days = Math.floor(diff / 86400000);
  if (days < 1) return "today";
  if (days < 7) return `${days}d ago`;
  if (days < 30) return `${Math.floor(days / 7)}w ago`;
  if (days < 365) return `${Math.floor(days / 30)}mo ago`;
  return `${Math.floor(days / 365)}y ago`;
}

function CopyButton({ text, label }: { text: string; label: string }) {
  const [copied, setCopied] = useState(false);

  const handleCopy = async () => {
    await navigator.clipboard.writeText(text);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  return (
    <button
      onClick={handleCopy}
      title={`Copy ${label}`}
      className="p-1.5 rounded-md text-muted-foreground hover:text-foreground hover:bg-muted transition-colors"
    >
      {copied ? (
        <AiOutlineCheck className="h-4 w-4 text-green-500" />
      ) : (
        <AiOutlineCopy className="h-4 w-4" />
      )}
    </button>
  );
}

function CodeSnippet({ label, code }: { label: string; code: string }) {
  return (
    <div className="flex flex-col gap-1">
      <span className="text-xs text-muted-foreground font-medium uppercase tracking-wide">
        {label}
      </span>
      <div className="flex items-center gap-2 bg-muted rounded-lg px-3 py-2 font-mono text-sm">
        <code className="flex-1 text-foreground overflow-x-auto whitespace-nowrap">
          {code}
        </code>
        <CopyButton text={code} label={label} />
      </div>
    </div>
  );
}

export function PackageCard({ pkg }: { pkg: PackageResult }) {
  return (
    <div className="group flex flex-col gap-4 rounded-xl border border-border bg-card p-5 hover:border-ring transition-all hover:shadow-sm">
      {/* Header */}
      <div className="flex items-start justify-between gap-3">
        <div className="flex items-center gap-3 min-w-0">
          <img
            src={pkg.ownerAvatar}
            alt={pkg.owner}
            className="h-8 w-8 rounded-full border border-border flex-shrink-0"
          />
          <div className="min-w-0">
            <div className="flex items-center gap-1.5 flex-wrap">
              <span className="text-muted-foreground text-sm">{pkg.owner}</span>
              <span className="text-muted-foreground">/</span>
              <span className="font-semibold text-foreground">{pkg.name}</span>
              {pkg.latestVersion && (
                <span className="flex items-center gap-1 text-xs bg-primary/10 text-primary border border-primary/20 rounded-full px-2 py-0.5 font-mono">
                  <AiOutlineTag className="h-3 w-3" />
                  {pkg.latestVersion}
                </span>
              )}
            </div>
          </div>
        </div>
        <a
          href={`https://github.com/${pkg.fullName}`}
          target="_blank"
          rel="noopener noreferrer"
          className="flex-shrink-0 text-muted-foreground hover:text-foreground transition-colors"
          title="View on GitHub"
        >
          <AiOutlineLink className="h-4 w-4" />
        </a>
      </div>

      {/* Description */}
      <p className="text-sm text-muted-foreground line-clamp-2 leading-relaxed">
        {pkg.description}
      </p>

      {/* Topics */}
      {pkg.topics.length > 0 && (
        <div className="flex flex-wrap gap-1.5">
          {pkg.topics.slice(0, 5).map((topic) => (
            <span
              key={topic}
              className="text-xs bg-secondary text-secondary-foreground rounded-full px-2.5 py-0.5"
            >
              {topic}
            </span>
          ))}
        </div>
      )}

      {/* Install snippets */}
      <div className="flex flex-col gap-2.5 pt-1">
        <CodeSnippet label="cpm add" code={pkg.cpmInstall} />
        <CodeSnippet label="cpm.toml" code={pkg.cpmToml} />
      </div>

      {/* Footer meta */}
      <div className="flex items-center gap-4 text-xs text-muted-foreground pt-1 border-t border-border">
        {pkg.language && (
          <span className="flex items-center gap-1">
            <span className="h-2.5 w-2.5 rounded-full bg-primary inline-block" />
            {pkg.language}
          </span>
        )}
        <span className="flex items-center gap-1">
          <AiOutlineStar className="h-3.5 w-3.5" />
          {pkg.stars.toLocaleString()}
        </span>
        <span className="flex items-center gap-1">
          <BiGitRepoForked className="h-3.5 w-3.5" />
          {pkg.forks.toLocaleString()}
        </span>
        {pkg.license && <span>{pkg.license}</span>}
        <span className="flex items-center gap-1 ml-auto">
          <AiOutlineClockCircle className="h-3.5 w-3.5" />
          {timeAgo(pkg.pushedAt)}
        </span>
      </div>
    </div>
  );
}
