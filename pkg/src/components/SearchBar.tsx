"use client";

import { useState, useCallback, useRef, useEffect, MutableRefObject } from "react";
import { AiOutlineSearch, AiOutlineClose, AiOutlineLoading3Quarters } from "react-icons/ai";
import type { PackageResult } from "@/app/api/search/route";

interface SearchBarProps {
  onResults: (results: PackageResult[], query: string) => void;
  onLoading: (loading: boolean) => void;
  onError: (error: string | null) => void;
  triggerRef?: MutableRefObject<((q: string) => void) | null>;
}

export function SearchBar({ onResults, onLoading, onError, triggerRef }: SearchBarProps) {
  const [query, setQuery] = useState("");
  const [isSearching, setIsSearching] = useState(false);
  const debounceRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const search = useCallback(
    async (q: string) => {
      if (!q.trim()) {
        onResults([], "");
        return;
      }

      setIsSearching(true);
      onLoading(true);
      onError(null);

      try {
        const res = await fetch(`/api/search?q=${encodeURIComponent(q)}`);
        const data = await res.json();

        if (!res.ok) {
          onError(data.error ?? "Search failed. Try again.");
          onResults([], q);
        } else {
          onResults(data.results, q);
        }
      } catch {
        onError("Network error. Check your connection.");
        onResults([], q);
      } finally {
        setIsSearching(false);
        onLoading(false);
      }
    },
    [onResults, onLoading, onError]
  );

  // Expose trigger for popular search buttons
  useEffect(() => {
    if (triggerRef) {
      triggerRef.current = (q: string) => {
        setQuery(q);
        if (debounceRef.current) clearTimeout(debounceRef.current);
        search(q);
      };
    }
  }, [triggerRef, search]);

  const handleChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const val = e.target.value;
    setQuery(val);
    if (debounceRef.current) clearTimeout(debounceRef.current);
    debounceRef.current = setTimeout(() => search(val), 500);
  };

  const handleClear = () => {
    setQuery("");
    onResults([], "");
    onError(null);
  };

  const handleSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (debounceRef.current) clearTimeout(debounceRef.current);
    search(query);
  };

  return (
    <form onSubmit={handleSubmit} className="w-full max-w-2xl mx-auto">
      <div className="relative flex items-center">
        <div className="absolute left-4 text-muted-foreground pointer-events-none">
          {isSearching ? (
            <AiOutlineLoading3Quarters className="h-5 w-5 animate-spin" />
          ) : (
            <AiOutlineSearch className="h-5 w-5" />
          )}
        </div>
        <input
          type="text"
          value={query}
          onChange={handleChange}
          placeholder="Search C/C++ packages... (e.g. json, http, redis)"
          className="w-full h-14 pl-12 pr-12 rounded-xl border border-border bg-card text-foreground placeholder:text-muted-foreground text-base focus:outline-none focus:ring-2 focus:ring-ring transition-all"
          autoFocus
        />
        {query && (
          <button
            type="button"
            onClick={handleClear}
            className="absolute right-4 text-muted-foreground hover:text-foreground transition-colors"
          >
            <AiOutlineClose className="h-5 w-5" />
          </button>
        )}
      </div>
    </form>
  );
}
