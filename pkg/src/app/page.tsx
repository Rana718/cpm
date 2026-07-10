"use client";

import { useState } from "react";
import { VscPackage } from "react-icons/vsc";
import { FaGithub } from "react-icons/fa";
import { HiOutlineBookOpen } from "react-icons/hi";
import { SearchBar } from "@/components/SearchBar";
import { PackageCard } from "@/components/PackageCard";
import type { PackageResult } from "@/app/api/search/route";

const POPULAR_SEARCHES = ["json", "http", "redis", "grpc", "boost", "fmt", "asio", "openssl", "yaml", "spdlog"];

export default function Home() {
  const [results, setResults] = useState<PackageResult[]>([]);
  const [query, setQuery] = useState("");
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [searched, setSearched] = useState(false);

  const handleResults = (res: PackageResult[], q: string) => {
    setResults(res);
    setQuery(q);
    if (q) setSearched(true);
  };

  return (
    <div className="min-h-screen flex flex-col">
      {/* Navbar */}
      <header className="border-b border-border bg-card/80 backdrop-blur sticky top-0 z-10">
        <div className="max-w-6xl mx-auto px-4 h-14 flex items-center justify-between">
          <div className="flex items-center gap-2">
            <VscPackage className="h-5 w-5 text-primary" />
            <span className="font-semibold text-foreground">cpm registry</span>
            <span className="text-xs bg-secondary text-secondary-foreground rounded-full px-2 py-0.5 font-mono">
              beta
            </span>
          </div>
          <div className="flex items-center gap-3">
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
        <section className="py-16 px-4 text-center border-b border-border bg-gradient-to-b from-muted/40 to-background">
          <div className="max-w-3xl mx-auto flex flex-col items-center gap-6">
            <div className="flex flex-col gap-2">
              <h1 className="text-4xl sm:text-5xl font-bold tracking-tight text-foreground font-heading">
                Find C/C++ Packages
              </h1>
              <p className="text-lg text-muted-foreground max-w-xl mx-auto">
                Search any library on GitHub and get the exact{" "}
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
            />

            {/* Popular searches */}
            {!searched && (
              <div className="flex flex-wrap justify-center gap-2 mt-2">
                <span className="text-sm text-muted-foreground">Popular:</span>
                {POPULAR_SEARCHES.map((term) => (
                  <button
                    key={term}
                    onClick={() => {
                      handleResults([], term);
                      // trigger search via a hidden form submit workaround
                      const event = new CustomEvent("cpm-search", { detail: term });
                      window.dispatchEvent(event);
                    }}
                    className="text-sm text-primary hover:underline"
                  >
                    {term}
                  </button>
                ))}
              </div>
            )}
          </div>
        </section>

        {/* Results */}
        <section className="flex-1 max-w-6xl w-full mx-auto px-4 py-10">
          {error && (
            <div className="mb-6 rounded-lg border border-destructive/30 bg-destructive/10 text-destructive px-4 py-3 text-sm">
              {error}
            </div>
          )}

          {loading && (
            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
              {Array.from({ length: 6 }).map((_, i) => (
                <div
                  key={i}
                  className="h-64 rounded-xl border border-border bg-card animate-pulse"
                />
              ))}
            </div>
          )}

          {!loading && searched && results.length === 0 && !error && (
            <div className="text-center py-20 text-muted-foreground">
              <VscPackage className="h-12 w-12 mx-auto mb-4 opacity-30" />
              <p className="text-lg font-medium">No packages found for &quot;{query}&quot;</p>
              <p className="text-sm mt-1">Try a different search term or check GitHub directly.</p>
            </div>
          )}

          {!loading && results.length > 0 && (
            <>
              <div className="flex items-center justify-between mb-6">
                <p className="text-sm text-muted-foreground">
                  Showing results for{" "}
                  <span className="font-medium text-foreground">&quot;{query}&quot;</span>
                </p>
                <span className="text-xs text-muted-foreground bg-muted rounded-full px-3 py-1">
                  {results.length} packages
                </span>
              </div>
              <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
                {results.map((pkg) => (
                  <PackageCard key={pkg.id} pkg={pkg} />
                ))}
              </div>
            </>
          )}

          {!searched && !loading && (
            <div className="text-center py-20 text-muted-foreground">
              <VscPackage className="h-16 w-16 mx-auto mb-4 opacity-20" />
              <p className="text-base">Start typing to search GitHub for C/C++ packages</p>
            </div>
          )}
        </section>
      </main>

      {/* Footer */}
      <footer className="border-t border-border py-6 text-center text-xs text-muted-foreground">
        <p>
          cpm registry — powered by{" "}
          <a
            href="https://docs.github.com/en/rest"
            target="_blank"
            rel="noopener noreferrer"
            className="hover:text-foreground underline"
          >
            GitHub API
          </a>{" "}
          · packages sourced from GitHub
        </p>
      </footer>
    </div>
  );
}
