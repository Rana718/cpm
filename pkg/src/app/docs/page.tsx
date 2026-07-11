"use client";

import { useState, useEffect } from "react";
import { useTheme } from "next-themes";
import { useRouter } from "next/navigation";
import { FaGithub } from "react-icons/fa";
import { Light as SyntaxHighlighter } from "react-syntax-highlighter";
import { androidstudio as darkStyle, xcode as lightStyle } from "react-syntax-highlighter/dist/esm/styles/hljs";
import bash from "react-syntax-highlighter/dist/esm/languages/hljs/bash";
import cpp from "react-syntax-highlighter/dist/esm/languages/hljs/cpp";
import c from "react-syntax-highlighter/dist/esm/languages/hljs/c";
import json from "react-syntax-highlighter/dist/esm/languages/hljs/json";
import yaml from "react-syntax-highlighter/dist/esm/languages/hljs/yaml";
import plaintext from "react-syntax-highlighter/dist/esm/languages/hljs/plaintext";

SyntaxHighlighter.registerLanguage("bash", bash);
SyntaxHighlighter.registerLanguage("shell", bash);
SyntaxHighlighter.registerLanguage("cpp", cpp);
SyntaxHighlighter.registerLanguage("c", c);
SyntaxHighlighter.registerLanguage("json", json);
SyntaxHighlighter.registerLanguage("yaml", yaml);
SyntaxHighlighter.registerLanguage("toml", plaintext);
SyntaxHighlighter.registerLanguage("text", plaintext);

import {
  AiOutlineCopy,
  AiOutlineCheck,
  AiOutlineDown,
  AiOutlineRight,
} from "react-icons/ai";
import {
  HiOutlineCommandLine,
  HiOutlineCog6Tooth,
  HiOutlineCube,
  HiOutlineBookOpen,
  HiOutlineRocketLaunch,
  HiOutlineWrenchScrewdriver,
} from "react-icons/hi2";

function CopyButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);
  return (
    <button
      onClick={async () => {
        await navigator.clipboard.writeText(text);
        setCopied(true);
        setTimeout(() => setCopied(false), 2000);
      }}
      className="p-1.5 rounded bg-white/10 hover:bg-white/20 transition-colors text-white/70 hover:text-white"
    >
      {copied ? <AiOutlineCheck className="h-3.5 w-3.5" /> : <AiOutlineCopy className="h-3.5 w-3.5" />}
    </button>
  );
}

function CodeBlock({ code, lang = "bash" }: { code: string; lang?: string }) {
  const { resolvedTheme } = useTheme();
  const [mounted, setMounted] = useState(false);
  useEffect(() => setMounted(true), []);
  const isDark = resolvedTheme === "dark";

  return (
    <div className="relative group my-3 rounded-lg overflow-hidden border border-border">
      <div className="flex items-center justify-end px-3 py-1.5 bg-zinc-800 dark:bg-zinc-900">
        <CopyButton text={code} />
      </div>
      {mounted ? (
        <SyntaxHighlighter
          style={isDark ? darkStyle : lightStyle}
          language={lang}
          PreTag="div"
          customStyle={{
            margin: 0,
            borderRadius: 0,
            fontSize: "0.85rem",
            padding: "1rem",
            background: isDark ? "#282b2e" : "#f8f8ff",
            msOverflowStyle: "none",
            scrollbarWidth: "none",
          }}
        >
          {code}
        </SyntaxHighlighter>
      ) : (
        <pre className="bg-muted px-4 py-4 text-sm font-mono overflow-x-auto [&::-webkit-scrollbar]:hidden">
          <code>{code}</code>
        </pre>
      )}
    </div>
  );
}

function Section({ id, children }: { id: string; children: React.ReactNode }) {
  return (
    <section id={id} className="scroll-mt-20">
      {children}
    </section>
  );
}

function Heading({ children }: { children: React.ReactNode }) {
  return (
    <h2 className="text-xl font-bold text-foreground mt-10 mb-4 border-b border-border pb-2">
      {children}
    </h2>
  );
}

function SubHeading({ children }: { children: React.ReactNode }) {
  return <h3 className="text-base font-semibold text-foreground mt-6 mb-2">{children}</h3>;
}

function Callout({ type, children }: { type: "tip" | "note" | "warn"; children: React.ReactNode }) {
  const styles = {
    tip: "bg-green-500/10 border-green-500/30 text-green-700 dark:text-green-400",
    note: "bg-blue-500/10 border-blue-500/30 text-blue-700 dark:text-blue-400",
    warn: "bg-yellow-500/10 border-yellow-500/30 text-yellow-700 dark:text-yellow-400",
  };
  const labels = { tip: "TIP", note: "NOTE", warn: "WARNING" };
  return (
    <div className={`border rounded-lg px-4 py-3 my-3 text-sm ${styles[type]}`}>
      <span className="font-bold mr-2">{labels[type]}:</span>
      {children}
    </div>
  );
}

function AccordionItem({ q, children }: { q: string; children: React.ReactNode }) {
  const [open, setOpen] = useState(false);
  return (
    <div className="border border-border rounded-lg overflow-hidden">
      <button
        onClick={() => setOpen(!open)}
        className="w-full flex items-center justify-between px-4 py-3 text-left text-sm font-medium text-foreground hover:bg-muted transition-colors"
      >
        {q}
        {open ? <AiOutlineDown className="h-4 w-4 flex-shrink-0" /> : <AiOutlineRight className="h-4 w-4 flex-shrink-0" />}
      </button>
      {open && (
        <div className="px-4 py-3 text-sm text-muted-foreground border-t border-border">
          {children}
        </div>
      )}
    </div>
  );
}

const NAV = [
  { id: "install", label: "Installation", icon: HiOutlineRocketLaunch },
  { id: "quickstart", label: "Quick Start", icon: HiOutlineCommandLine },
  { id: "config", label: "cpm.toml", icon: HiOutlineCog6Tooth },
  { id: "commands", label: "Commands", icon: HiOutlineCommandLine },
  { id: "dependencies", label: "Dependencies", icon: HiOutlineCube },
  { id: "build", label: "Build & Run", icon: HiOutlineWrenchScrewdriver },
  { id: "faq", label: "FAQ", icon: HiOutlineBookOpen },
];

const COMMANDS = [
  { cmd: "cpm init <name>", desc: "Create a new C/C++ project" },
  { cmd: "cpm install", desc: "Install all dependencies from cpm.toml" },
  { cmd: "cpm build", desc: "Compile the project" },
  { cmd: "cpm build -s", desc: "Production build — optimized + portable bundle in dist/" },
  { cmd: "cpm run", desc: "Install + build + run in one command" },
  { cmd: "cpm run file.cpp", desc: "Compile and run a single file (no project needed)" },
  { cmd: "cpm start", desc: "Run the already-built binary" },
  { cmd: "cpm add <pkg>", desc: "Add a package, e.g. cpm add github:nlohmann/json" },
  { cmd: "cpm remove <name>", desc: "Remove a package by name" },
  { cmd: "cpm update", desc: "Re-download all dependencies" },
  { cmd: "cpm list", desc: "Show installed packages" },
  { cmd: "cpm setup", desc: "Install the Nix backend" },
  { cmd: "cpm --version", desc: "Show cpm version" },
];

export default function DocsPage() {
  const router = useRouter();
  const [activeSection, setActiveSection] = useState("install");

  const scrollTo = (id: string) => {
    setActiveSection(id);
    document.getElementById(id)?.scrollIntoView({ behavior: "smooth" });
  };

  return (
    <div className="flex flex-1 overflow-x-hidden">
      {/* Sidebar — fixed below the layout navbar */}
      <aside className="hidden lg:flex flex-col w-60 flex-shrink-0 border-r border-border fixed top-14 left-0 h-[calc(100vh-3.5rem)] py-6 px-4 bg-background z-40">
          <p className="text-xs font-semibold text-muted-foreground uppercase tracking-wider mb-3 px-2">
            Documentation
          </p>
          <nav className="flex flex-col gap-0.5">
            {NAV.map(({ id, label, icon: Icon }) => (
              <button
                key={id}
                onClick={() => scrollTo(id)}
                className={`flex items-center gap-2.5 px-2 py-2 rounded-lg text-sm transition-colors text-left ${
                  activeSection === id
                    ? "bg-primary/10 text-primary font-medium"
                    : "text-muted-foreground hover:text-foreground hover:bg-muted"
                }`}
              >
                <Icon className="h-4 w-4 flex-shrink-0" />
                {label}
              </button>
            ))}
          </nav>
        </aside>

        {/* Main content — offset by sidebar width on large screens */}
        <main className="flex-1 min-w-0 px-8 lg:pl-[calc(15rem+2rem)] lg:pr-12 py-10 max-w-4xl">
          {/* Hero */}
          <div className="mb-10">
            <h1 className="text-3xl font-bold text-foreground font-heading mb-2">
              cpm — C/C++ Package Manager
            </h1>
            <p className="text-muted-foreground text-base">
              A fast, isolated package manager for C and C++ projects. Like{" "}
              <code className="bg-muted px-1.5 py-0.5 rounded text-sm font-mono">uv</code> for Python, but for C++.
            </p>
          </div>

          {/* Installation */}
          <Section id="install">
            <Heading>Installation</Heading>
            <SubHeading>One-line install (Linux)</SubHeading>
            <CodeBlock code="curl -fsSL https://raw.githubusercontent.com/Rana718/cpm/main/install.sh | bash" />
            <p className="text-sm text-muted-foreground">This installs system build tools (cmake, ninja, g++), Nix for isolated builds, and the cpm binary to <code className="bg-muted px-1 rounded font-mono text-xs">/usr/local/bin/</code>.</p>

            <SubHeading>From source</SubHeading>
            <CodeBlock code={`git clone https://github.com/Rana718/cpm.git
cd cpm
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
sudo cp cpm /usr/local/bin/`} />

            <SubHeading>Verify</SubHeading>
            <CodeBlock code="cpm --version" />
            <Callout type="note">cpm requires Linux x86_64, git, curl, and Nix (auto-installed).</Callout>
          </Section>

          {/* Quick Start */}
          <Section id="quickstart">
            <Heading>Quick Start</Heading>
            <CodeBlock code={`# Create a new project
mkdir myapp && cd myapp
cpm init myapp

# Add a dependency
cpm add github:nlohmann/json

# Build and run
cpm run`} />
            <p className="text-sm text-muted-foreground mt-2">
              <code className="bg-muted px-1 rounded font-mono text-xs">cpm init</code> creates a project with a <code className="bg-muted px-1 rounded font-mono text-xs">cpm.toml</code>, <code className="bg-muted px-1 rounded font-mono text-xs">main.cpp</code>, and editor support via <code className="bg-muted px-1 rounded font-mono text-xs">compile_commands.json</code>.
            </p>

            <SubHeading>Run a single file</SubHeading>
            <CodeBlock code={`# No project needed — uses system g++
cpm run hello.cpp
cpm run test.c`} />
          </Section>

          {/* cpm.toml */}
          <Section id="config">
            <Heading>cpm.toml</Heading>
            <p className="text-sm text-muted-foreground mb-3">The project manifest — equivalent to <code className="bg-muted px-1 rounded font-mono text-xs">package.json</code> or <code className="bg-muted px-1 rounded font-mono text-xs">pyproject.toml</code>.</p>
            <CodeBlock lang="toml" code={`[project]
name = "myapp"
version = "0.1.0"
cpp_standard = "20"    # 11, 14, 17, 20, 23, 26
compiler = "gcc-13"    # optional: gcc, gcc-13, clang-17
entry = "main.cpp"
output = "myapp"

[scripts]
start = "./myapp"

[dependencies]
# Header-only libraries (fast — just clones the repo)
json = "github:nlohmann/json@v3.11.3"
fmt  = "github:fmtlib/fmt"            # latest tag

[system-dependencies]
# Compiled libraries (built inside nix-shell)
hiredis = "github:redis/hiredis"
seastar = "github:scylladb/seastar"`} />

            <SubHeading>Dependency formats</SubHeading>
            <div className="overflow-x-auto rounded-lg border border-border my-3">
              <table className="w-full text-sm">
                <thead>
                  <tr className="bg-muted border-b border-border">
                    <th className="text-left px-4 py-2 font-medium text-muted-foreground">Format</th>
                    <th className="text-left px-4 py-2 font-medium text-muted-foreground">Example</th>
                    <th className="text-left px-4 py-2 font-medium text-muted-foreground">Description</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-border">
                  {[
                    ["github:owner/repo", "github:nlohmann/json", "Latest default branch"],
                    ["github:owner/repo@tag", "github:fmtlib/fmt@10.2.1", "Pinned version"],
                    ["github:owner/repo@branch", "github:owner/repo@main", "Specific branch"],
                  ].map(([fmt, ex, desc]) => (
                    <tr key={fmt} className="hover:bg-muted/30">
                      <td className="px-4 py-2 font-mono text-xs text-foreground">{fmt}</td>
                      <td className="px-4 py-2 font-mono text-xs text-primary">{ex}</td>
                      <td className="px-4 py-2 text-muted-foreground">{desc}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </Section>

          {/* Commands */}
          <Section id="commands">
            <Heading>Commands</Heading>
            <div className="flex flex-col gap-2">
              {COMMANDS.map(({ cmd, desc }) => (
                <div key={cmd} className="flex flex-col sm:flex-row sm:items-center gap-1 sm:gap-4 rounded-lg border border-border px-4 py-3 hover:bg-muted/30 transition-colors">
                  <code className="font-mono text-sm text-primary whitespace-nowrap flex-shrink-0">{cmd}</code>
                  <span className="text-sm text-muted-foreground">{desc}</span>
                </div>
              ))}
            </div>
          </Section>

          {/* Dependencies */}
          <Section id="dependencies">
            <Heading>Dependencies</Heading>

            <SubHeading>Header-only libraries</SubHeading>
            <p className="text-sm text-muted-foreground mb-2">
              These are git-cloned and symlinked into <code className="bg-muted px-1 rounded font-mono text-xs">.cpm/include/</code>. Fast — no compilation. Add under <code className="bg-muted px-1 rounded font-mono text-xs">[dependencies]</code>.
            </p>
            <CodeBlock lang="toml" code={`[dependencies]
json = "github:nlohmann/json@v3.11.3"
fmt  = "github:fmtlib/fmt"`} />

            <SubHeading>Compiled / system libraries</SubHeading>
            <p className="text-sm text-muted-foreground mb-2">
              Built from source inside <code className="bg-muted px-1 rounded font-mono text-xs">nix-shell</code>. Provides correct compiler + all system deps. Add under <code className="bg-muted px-1 rounded font-mono text-xs">[system-dependencies]</code>.
            </p>
            <CodeBlock lang="toml" code={`[system-dependencies]
hiredis = "github:redis/hiredis"
seastar = "github:scylladb/seastar"`} />

            <Callout type="tip">
              Not sure which to use? If the library is header-only (e.g. nlohmann/json, Eigen, spdlog) use <code className="bg-muted px-1 rounded font-mono text-xs">[dependencies]</code>. If it needs to be compiled (hiredis, grpc, seastar) use <code className="bg-muted px-1 rounded font-mono text-xs">[system-dependencies]</code>.
            </Callout>

            <SubHeading>Where files go</SubHeading>
            <CodeBlock lang="text" code={`.cpm/
├── include/        ← symlinked headers
├── lib/            ← compiled .so files
├── lib-static/     ← compiled .a files
├── packages/       ← cloned repos
└── defines.txt     ← compiler flags`} />
          </Section>

          {/* Build & Run */}
          <Section id="build">
            <Heading>Build & Run</Heading>

            <SubHeading>Development</SubHeading>
            <CodeBlock code={`cpm run          # install + build + run
cpm build        # compile only
cpm start        # run existing binary`} />

            <SubHeading>Production build</SubHeading>
            <CodeBlock code="cpm build -s" />
            <p className="text-sm text-muted-foreground mb-2">Produces a <code className="bg-muted px-1 rounded font-mono text-xs">dist/</code> folder:</p>
            <CodeBlock lang="text" code={`dist/
├── myapp       ← optimized, stripped binary
├── lib*.so     ← bundled shared libraries
└── run.sh      ← portable launcher`} />
            <p className="text-sm text-muted-foreground">Copy <code className="bg-muted px-1 rounded font-mono text-xs">dist/</code> to any Linux server and run <code className="bg-muted px-1 rounded font-mono text-xs">./dist/run.sh</code>.</p>

            <SubHeading>Multi-file projects</SubHeading>
            <p className="text-sm text-muted-foreground">Put additional source files in a <code className="bg-muted px-1 rounded font-mono text-xs">src/</code> directory. cpm automatically picks them up.</p>
            <CodeBlock lang="text" code={`myapp/
├── cpm.toml
├── main.cpp
└── src/
    ├── server.cpp
    └── handlers.hpp`} />
          </Section>

          {/* FAQ */}
          <Section id="faq">
            <Heading>FAQ</Heading>
            <div className="flex flex-col gap-2">
              <AccordionItem q="Does cpm work on macOS or Windows?">
                Currently Linux x86_64 only. macOS and Windows support is planned.
              </AccordionItem>
              <AccordionItem q="Will cpm touch my system libraries?">
                No. Everything is stored in <code className="bg-muted px-1 rounded font-mono text-xs">.cpm/</code> (local) and <code className="bg-muted px-1 rounded font-mono text-xs">~/.cpm/cache/</code> (global). <code className="bg-muted px-1 rounded font-mono text-xs">/usr/local</code> is never touched.
              </AccordionItem>
              <AccordionItem q="Why does cpm use Nix?">
                Nix provides pre-built binaries from <code className="bg-muted px-1 rounded font-mono text-xs">cache.nixos.org</code> — most compiled deps don't need local compilation. It also guarantees the correct compiler version and reproducible builds.
              </AccordionItem>
              <AccordionItem q="How do I use a package without a tagged release?">
                Use the branch name: <code className="bg-muted px-1 rounded font-mono text-xs">github:owner/repo@main</code>
              </AccordionItem>
              <AccordionItem q="How do I find the right package name for cpm.toml?">
                Search on this registry site, then copy the snippet directly from the Install tab on the package page.
              </AccordionItem>
              <AccordionItem q="Can I use cpm in CI/CD?">
                Yes — run <code className="bg-muted px-1 rounded font-mono text-xs">cpm setup</code> first to install Nix, then <code className="bg-muted px-1 rounded font-mono text-xs">cpm build</code> as normal.
              </AccordionItem>
            </div>
          </Section>

          <div className="mt-16 pt-6 border-t border-border text-sm text-muted-foreground flex items-center justify-between flex-wrap gap-2">
            <span>cpm is open source — MIT License</span>
            <a
              href="https://github.com/Rana718/cpm"
              target="_blank"
              rel="noopener noreferrer"
              className="flex items-center gap-1.5 hover:text-foreground transition-colors"
            >
              <FaGithub className="h-4 w-4" />
              Contribute on GitHub
            </a>
          </div>
        </main>
      </div>
  );
}
