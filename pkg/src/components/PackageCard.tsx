"use client";

import { useRouter } from "next/navigation";
import {
  AiOutlineStar,
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

export function PackageCard({ pkg }: { pkg: PackageResult }) {
  const router = useRouter();

  const handleClick = () => {
    // Store current scroll position so back feels instant
    sessionStorage.setItem("scrollY", String(window.scrollY));
    router.push(`/package/${pkg.owner}/${pkg.name}`);
  };

  return (
    <div
      onClick={handleClick}
      className="group flex flex-col gap-3 rounded-xl border border-border bg-card p-5 hover:border-ring transition-all hover:shadow-md cursor-pointer"
    >
      {/* Header */}
      <div className="flex items-start gap-3">
        <img
          src={pkg.ownerAvatar}
          alt={pkg.owner}
          className="h-9 w-9 rounded-full border border-border flex-shrink-0 mt-0.5"
        />
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-1.5 flex-wrap">
            <span className="text-muted-foreground text-sm">{pkg.owner}</span>
            <span className="text-muted-foreground">/</span>
            <span className="font-semibold text-foreground group-hover:text-primary transition-colors">
              {pkg.name}
            </span>
          </div>
          {pkg.latestVersion && (
            <span className="inline-flex items-center gap-1 text-xs bg-primary/10 text-primary border border-primary/20 rounded-full px-2 py-0.5 font-mono mt-1">
              <AiOutlineTag className="h-3 w-3" />
              {pkg.latestVersion}
            </span>
          )}
        </div>
      </div>

      {/* Description */}
      <p className="text-sm text-muted-foreground line-clamp-2 leading-relaxed">
        {pkg.description}
      </p>

      {/* Topics */}
      {pkg.topics.length > 0 && (
        <div className="flex flex-wrap gap-1.5">
          {pkg.topics.slice(0, 4).map((topic) => (
            <span
              key={topic}
              className="text-xs bg-secondary text-secondary-foreground rounded-full px-2.5 py-0.5"
            >
              {topic}
            </span>
          ))}
        </div>
      )}

      {/* Footer meta */}
      <div className="flex items-center gap-3 text-xs text-muted-foreground pt-2 border-t border-border mt-auto">
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
        {pkg.license && <span className="hidden sm:inline">{pkg.license}</span>}
        <span className="flex items-center gap-1 ml-auto">
          <AiOutlineClockCircle className="h-3.5 w-3.5" />
          {timeAgo(pkg.pushedAt)}
        </span>
      </div>
    </div>
  );
}
