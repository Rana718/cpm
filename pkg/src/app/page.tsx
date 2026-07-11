"use client";

import { useState, useRef, useCallback, useEffect, Suspense } from "react";
import { useSearchParams } from "next/navigation";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { VscPackage } from "react-icons/vsc";
import { PackageCard } from "@/components/PackageCard";
import type { PackageResult } from "@/app/api/search/route";
import { AiOutlineSearch, AiOutlineClose, AiOutlineLoading3Quarters } from "react-icons/ai";

const POPULAR_SEARCHES = [
  "json", "http", "redis", "grpc", "boost", "fmt",
  "asio", "openssl", "yaml", "spdlog", "curl", "protobuf",
];

async function fetchSearch(query: string): Promise<{ results: PackageResult[]; total: number }> {
  const res = await fetch(`/api/search?q=${encodeURIComponent(query)}`);
  const data = await res.json();
  if (!res.ok) throw new Error(data.error ?? "Search failed");
  return data;
}

function HomeContent() {
  const searchParams = useSearchParams();
  const queryClient = useQueryClient();

  const initialQuery = searchParams.get("q") ?? "";
  const [inputValue, setInputValue] = useState(initialQuery);
  const [activeQuery, setActiveQuery] = useState(initialQuery);
  const debounceRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Restore scroll after back navigation
  useEffect(() => {
    const savedY = sessionStorage.getItem("scrollY");
    if (savedY) {
      setTimeout(() => window.scrollTo({ top: parseInt(savedY), behavior: "instant" }), 50);
      sessionStorage.removeItem("scrollY");
    }
  }, []);

  // Keep URL in sync
  useEffect(() => {
    const newUrl = activeQuery
      ? `${window.location.pathname}?q=${encodeURIComponent(activeQuery)}`
      : window.location.pathname;
    window.history.replaceState(null, "", newUrl);
  }, [activeQuery]);

  const { data, isFetching, error } = useQuery({
    queryKey: ["search", activeQuery],
    queryFn: () => fetchSearch(activeQuery),
    enabled: activeQuery.length > 0,
    staleTime: 1000 * 60 * 5,
    gcTime: 1000 * 60 * 30,
  });

  const triggerSearch = useCallback((q: string) => {
    setInputValue(q);
    if (debounceRef.current) clearTimeout(debounceRef.current);
    if (!q.trim()) { setActiveQuery(""); return; }
    debounceRef.current = setTimeout(() => setActiveQuery(q.trim()), 500);
  }, []);

  const handlePopular = (term: string) => {
    setInputValue(term);
    setActiveQuery(term);
    queryClient.prefetchQuery({
      queryKey: ["search", term],
      queryFn: () => fetchSearch(term),
      staleTime: 1000 * 60 * 5,
    });
  };

  const results = data?.results ?? [];
  const searched = activeQuery.length > 0;

  return (
    <div className="flex flex-col flex-1">
      {/* Hero search section */}
      <section className="py-14 px-4 text-center border-b border-border bg-gradient-to-b from-muted/40 to-background">
        <div className="max-w-3xl mx-auto flex flex-col items-center gap-6">
          <div className="flex flex-col gap-2">
            <h1 className="text-3xl sm:text-4xl lg:text-5xl font-bold tracking-tight text-foreground font-heading">
              Find C/C++ Packages
            </h1>
            <p className="text-base sm:text-lg text-muted-foreground max-w-xl mx-auto">
              Search any library and get the exact{" "}
              <code className="bg-muted px-1.5 py-0.5 rounded text-sm font-mono">cpm add</code>{" "}
              command and{" "}
              <code className="bg-muted px-1.5 py-0.5 rounded text-sm font-mono">cpm.toml</code>{" "}
              snippet — ready to copy.
            </p>
          </div>

          <div className="w-full max-w-2xl">
            <div className="relative flex items-center">
              <div className="absolute left-4 text-muted-foreground pointer-events-none">
                {isFetching
                  ? <AiOutlineLoading3Quarters className="h-5 w-5 animate-spin" />
                  : <AiOutlineSearch className="h-5 w-5" />}
              </div>
              <input
                type="text"
                value={inputValue}
                onChange={(e) => triggerSearch(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter") {
                    if (debounceRef.current) clearTimeout(debounceRef.current);
                    setActiveQuery(inputValue.trim());
                  }
                }}
                placeholder="Search C/C++ packages... (e.g. json, http, redis)"
                className="w-full h-14 pl-12 pr-12 rounded-xl border border-border bg-card text-foreground placeholder:text-muted-foreground text-base focus:outline-none focus:ring-2 focus:ring-ring transition-all"
                autoFocus
              />
              {inputValue && (
                <button
                  onClick={() => { setInputValue(""); setActiveQuery(""); }}
                  className="absolute right-4 text-muted-foreground hover:text-foreground transition-colors"
                >
                  <AiOutlineClose className="h-5 w-5" />
                </button>
              )}
            </div>
          </div>

          <div className="flex flex-wrap justify-center gap-x-3 gap-y-2">
            <span className="text-sm text-muted-foreground">Popular:</span>
            {POPULAR_SEARCHES.map((term) => (
              <button key={term} onClick={() => handlePopular(term)} className="text-sm text-primary hover:underline">
                {term}
              </button>
            ))}
          </div>
        </div>
      </section>

      {/* Results */}
      <section className="flex-1 w-full px-10 py-10">
        {error && (
          <div className="mb-6 rounded-lg border border-destructive/30 bg-destructive/10 text-destructive px-4 py-3 text-sm">
            {(error as Error).message}
          </div>
        )}

        {isFetching && (
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-4">
            {Array.from({ length: 8 }).map((_, i) => (
              <div key={i} className="h-52 rounded-xl border border-border bg-card animate-pulse" />
            ))}
          </div>
        )}

        {!isFetching && searched && results.length === 0 && !error && (
          <div className="text-center py-20 text-muted-foreground">
            <VscPackage className="h-12 w-12 mx-auto mb-4 opacity-30" />
            <p className="text-lg font-medium">No C/C++ packages found for &quot;{activeQuery}&quot;</p>
            <p className="text-sm mt-1">Try a different search term.</p>
          </div>
        )}

        {!isFetching && results.length > 0 && (
          <>
            <div className="flex items-center justify-between mb-6">
              <p className="text-sm text-muted-foreground">
                Results for <span className="font-medium text-foreground">&quot;{activeQuery}&quot;</span>
              </p>
              <span className="text-xs text-muted-foreground bg-muted rounded-full px-3 py-1">
                {results.length} packages
              </span>
            </div>
            <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-4">
              {results.map((pkg) => (
                <PackageCard key={pkg.id} pkg={pkg} />
              ))}
            </div>
          </>
        )}

        {!searched && !isFetching && (
          <div className="text-center py-20 text-muted-foreground">
            <VscPackage className="h-16 w-16 mx-auto mb-4 opacity-20" />
            <p className="text-base">Search for any C/C++ library above</p>
          </div>
        )}
      </section>

      <footer className="border-t border-border py-6 text-center text-xs text-muted-foreground">
        cpm registry — powered by{" "}
        <a href="https://docs.github.com/en/rest" target="_blank" rel="noopener noreferrer" className="hover:text-foreground underline">
          GitHub API
        </a>
      </footer>
    </div>
  );
}

export default function Home() {
  return <Suspense><HomeContent /></Suspense>;
}
