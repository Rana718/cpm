"use client";

import { useRouter, usePathname } from "next/navigation";
import { FaGithub } from "react-icons/fa";
import { HiOutlineBookOpen, HiOutlineMagnifyingGlass } from "react-icons/hi2";
import { ThemeToggle } from "@/components/ThemeToggle";

export function Navbar() {
  const router = useRouter();
  const pathname = usePathname();

  const navLinks = [
    {
      label: "Search",
      href: "/",
      icon: HiOutlineMagnifyingGlass,
      active: pathname === "/",
      onClick: () => router.push("/"),
    },
    {
      label: "Docs",
      href: "/docs",
      icon: HiOutlineBookOpen,
      active: pathname.startsWith("/docs"),
    },
  ];

  return (
    <header className="sticky top-0 z-50 w-full border-b border-border/60 bg-background/80 backdrop-blur-md">
      <div className="w-full px-6 sm:px-10 h-14 flex items-center justify-between gap-4">

        {/* Logo */}
        <button
          onClick={() => router.push("/")}
          className="flex items-center gap-2.5 group flex-shrink-0"
        >
          <img
            src="/icon.png"
            alt="cpm"
            className="h-8 w-8 rounded-lg shadow-sm group-hover:scale-105 transition-transform"
          />
          <div className="flex items-center gap-2">
            <span className="font-bold text-base text-foreground tracking-tight">
              cpm
            </span>
            <span className="text-muted-foreground font-normal text-base hidden sm:inline">
              registry
            </span>
            <span className="text-[10px] font-mono bg-primary/10 text-primary border border-primary/20 rounded-full px-1.5 py-0.5 hidden sm:inline leading-none">
              beta
            </span>
          </div>
        </button>

        {/* Right side */}
        <div className="flex items-center gap-1">
          {/* Nav links */}
          {navLinks.map(({ label, href, icon: Icon, active, onClick }) => (
            <a
              key={href}
              href={href}
              onClick={onClick ? (e) => { e.preventDefault(); onClick(); } : undefined}
              className={`flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-sm font-medium transition-colors ${
                active
                  ? "bg-muted text-foreground"
                  : "text-muted-foreground hover:text-foreground hover:bg-muted/60"
              }`}
            >
              <Icon className="h-4 w-4 flex-shrink-0" />
              <span className="hidden sm:inline">{label}</span>
            </a>
          ))}

          {/* Divider */}
          <div className="w-px h-5 bg-border mx-1 hidden sm:block" />

          {/* GitHub */}
          <a
            href="https://github.com/Rana718/cpm"
            target="_blank"
            rel="noopener noreferrer"
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-lg text-sm text-muted-foreground hover:text-foreground hover:bg-muted/60 transition-colors"
          >
            <FaGithub className="h-4 w-4 flex-shrink-0" />
            <span className="hidden sm:inline">GitHub</span>
          </a>

          {/* Theme toggle */}
          <ThemeToggle />
        </div>
      </div>
    </header>
  );
}
