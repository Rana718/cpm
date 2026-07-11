"use client";

import { useTheme } from "next-themes";
import { useEffect, useState } from "react";
import { HiOutlineSun, HiOutlineMoon } from "react-icons/hi2";

export function ThemeToggle() {
  const { theme, setTheme } = useTheme();
  const [mounted, setMounted] = useState(false);
  useEffect(() => setMounted(true), []);
  if (!mounted) return <div className="h-8 w-8" />;

  return (
    <button
      onClick={() => setTheme(theme === "dark" ? "light" : "dark")}
      className="h-8 w-8 flex items-center justify-center rounded-lg text-muted-foreground hover:text-foreground hover:bg-muted transition-colors"
      title="Toggle theme"
    >
      {theme === "dark" ? (
        <HiOutlineSun className="h-4 w-4" />
      ) : (
        <HiOutlineMoon className="h-4 w-4" />
      )}
    </button>
  );
}
