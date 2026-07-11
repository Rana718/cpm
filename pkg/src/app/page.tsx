"use client";

import { useState, useRef } from "react";
import { VscPackage } from "react-icons/vsc";
import { FaGithub } from "react-icons/fa";
import { HiOutlineBookOpen } from "react-icons/hi";
import { SearchBar } from "@/components/SearchBar";
import { PackageCard } from "@/components/PackageCard";
import type { PackageResult } from "@/app/api/search/route";

const POPULAR_SEARCHES = [
  "json", "http", "redis", "grpc", "boost", "fmt",
  "asio", "openssl", "yaml", "spdlog", "curl", "protobuf",
];

export default function Home() {
  const [results, setResults] = useState<PackageResult[]>([]);
  const [query, setQuery] = useState("");
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [searched, setSearched] = useState(false);
  const triggerRef = useRef<((q: string) => void) | null>(null);

  const handleResults = (res: PackageResult[], q: string) => {
    setResults(res);
    setQuery(q);
    if (q) setSearched(true);
  };

  const handlePopular = (term: string) => {
    if (triggerRef.current) triggerRef.current(term);
  };

  return (
    <div className="min-h-screen flex flex-col overflow-x-hidden">
      {/* Navbar */}
      <header className="border-b border-border bg-card/80 backdrop-blur sticky top-0 z-10">
        <div className="w-full px-10 h-14 flex items-center justify-between">
          <div className="flex items-center gap-2">
            <VscPackage className="h-5 w-5 text-primary" />
            <span className="font-semibold text-foreground">cpm registry</span>
            <span className="text-xs bg-secondary text-secondary-foreground rounded-full px-2 py-0.5 font-mono hidden sm:inline">
              beta
            </span>
          </div>
          <div className="flex items-center gap-4">
            <a
              href="https://github.com/Rana718/cpm"
              target="_blank"
              rel="noopener noreferrer"
              className="flex items-center gap-1.5 text-sm text-muted-foreground hover:text-foreground transition-colors"
            >
              <FaGithub className="h-4 w-4" />
              <span className="hidden sm:inline">GitHub</span>
            </a>
            <a
              href="https://github.com/Rana718/cpm#readme"
              target="_blank"
              rel="noopener noreferrer"
              className="flex items-center gap-1.5 text-sm text-muted-foreground hover:text-foreground transition-colors"
            >
              <HiOutlineBookOpen className="h-4 w-4" />
              <span className="hidden sm:inline">Docs</span>
            </a>
          </div>
        </div>
      </header>

      <main className="flex-1 flex flex-col">
        {/* Hero */}
        <section className="py-14 px-4 sm:px-6 text-center border-b border-border bg-gradient-to-b from-muted/40 to-background">
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

            <SearchBar
              onResults={handleResults}
              onLoading={setLoading}
              onError={setError}
              triggerRef={triggerRef}
            />

            {/* Popular searches */}
            <div className="flex flex-wrap justify-center gap-x-3 gap-y-2 mt-1">
              <span className="text-sm text-muted-foreground">Popular:</span>
              {POPULAR_SEARCHES.map((term) => (
                <button
                  key={term}
                  onClick={() => handlePopular(term)}
                  className="text-sm text-primary hover:underline"
                >
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
              {error}
            </div>
          )}

          {loading && (
            <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-4">
              {Array.from({ length: 8 }).map((_, i) => (
                <div key={i} className="h-52 rounded-xl border border-border bg-card animate-pulse" />
              ))}
            </div>
          )}

          {!loading && searched && results.length === 0 && !error && (
            <div className="text-center py-20 text-muted-foreground">
              <VscPackage className="h-12 w-12 mx-auto mb-4 opacity-30" />
              <p className="text-lg font-medium">No C/C++ packages found for &quot;{query}&quot;</p>
              <p className="text-sm mt-1">Try a different search term.</p>
            </div>
          )}

          {!loading && results.length > 0 && (
            <>
              <div className="flex items-center justify-between mb-6">
                <p className="text-sm text-muted-foreground">
                  Results for{" "}
                  <span className="font-medium text-foreground">&quot;{query}&quot;</span>
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

          {!searched && !loading && (
            <div className="text-center py-20 text-muted-foreground">
              <VscPackage className="h-16 w-16 mx-auto mb-4 opacity-20" />
              <p className="text-base">Search for any C/C++ library above</p>
            </div>
          )}
        </section>
      </main>

      <footer className="border-t border-border py-6 text-center text-xs text-muted-foreground">
        cpm registry — powered by{" "}
        <a
          href="https://docs.github.com/en/rest"
          target="_blank"
          rel="noopener noreferrer"
          className="hover:text-foreground underline"
        >
          GitHub API
        </a>
      </footer>
    </div>
  );
}
