"use client";

import { useState, useEffect } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import rehypeRaw from "rehype-raw";
import rehypeSanitize, { defaultSchema } from "rehype-sanitize";
import { useTheme } from "next-themes";
import type { Components } from "react-markdown";
import { AiOutlineCopy, AiOutlineCheck } from "react-icons/ai";

import { Light as SyntaxHighlighter } from "react-syntax-highlighter";
import { androidstudio as darkStyle, xcode as lightStyle } from "react-syntax-highlighter/dist/esm/styles/hljs";
import bash from "react-syntax-highlighter/dist/esm/languages/hljs/bash";
import cpp from "react-syntax-highlighter/dist/esm/languages/hljs/cpp";
import c from "react-syntax-highlighter/dist/esm/languages/hljs/c";
import json from "react-syntax-highlighter/dist/esm/languages/hljs/json";
import yaml from "react-syntax-highlighter/dist/esm/languages/hljs/yaml";
import cmake from "react-syntax-highlighter/dist/esm/languages/hljs/cmake";
import plaintext from "react-syntax-highlighter/dist/esm/languages/hljs/plaintext";

SyntaxHighlighter.registerLanguage("bash", bash);
SyntaxHighlighter.registerLanguage("shell", bash);
SyntaxHighlighter.registerLanguage("sh", bash);
SyntaxHighlighter.registerLanguage("zsh", bash);
SyntaxHighlighter.registerLanguage("cpp", cpp);
SyntaxHighlighter.registerLanguage("c", c);
SyntaxHighlighter.registerLanguage("json", json);
SyntaxHighlighter.registerLanguage("yaml", yaml);
SyntaxHighlighter.registerLanguage("cmake", cmake);
SyntaxHighlighter.registerLanguage("toml", plaintext);
SyntaxHighlighter.registerLanguage("text", plaintext);
SyntaxHighlighter.registerLanguage("plaintext", plaintext);

const sanitizeSchema = {
  ...defaultSchema,
  attributes: {
    ...defaultSchema.attributes,
    "*": [...(defaultSchema.attributes?.["*"] ?? []), "style", "align", "className"],
    img: ["src", "alt", "title", "width", "height", "align", "style"],
    a: ["href", "title", "target", "rel"],
    code: ["className"],
    pre: ["className"],
  },
};

function normalizeLang(lang: string): string {
  const map: Record<string, string> = {
    sh: "bash", shell: "bash", zsh: "bash",
    "c++": "cpp",
    yml: "yaml",
    txt: "text", "": "text",
  };
  return map[lang.toLowerCase()] ?? lang.toLowerCase();
}

function CopyCode({ code }: { code: string }) {
  const [copied, setCopied] = useState(false);
  return (
    <button
      onClick={async () => {
        await navigator.clipboard.writeText(code);
        setCopied(true);
        setTimeout(() => setCopied(false), 2000);
      }}
      className="p-1.5 rounded bg-white/10 hover:bg-white/20 transition-colors text-white/70 hover:text-white"
    >
      {copied ? <AiOutlineCheck className="h-3.5 w-3.5" /> : <AiOutlineCopy className="h-3.5 w-3.5" />}
    </button>
  );
}

interface MarkdownRendererProps {
  content: string;
  owner: string;
  repo: string;
  branch: string;
}

export function MarkdownRenderer({ content, owner, repo, branch }: MarkdownRendererProps) {
  const { resolvedTheme } = useTheme();
  const [mounted, setMounted] = useState(false);
  useEffect(() => setMounted(true), []);

  const isDark = resolvedTheme === "dark";

  const fixedContent = content.replace(
    /!\[([^\]]*)\]\((?!https?:\/\/|#)([^)]+)\)/g,
    (_, alt, src) => {
      const clean = src.replace(/^\.\//, "");
      return `![${alt}](https://raw.githubusercontent.com/${owner}/${repo}/${branch}/${clean})`;
    }
  );

  const components: Components = {
    code({ className, children }) {
      const match = /language-(\w+)/.exec(className ?? "");
      const rawCode = String(children).replace(/\n$/, "");
      const isBlock = rawCode.includes("\n") || !!match;

      if (!isBlock) {
        return (
          <code className="bg-muted text-foreground px-1.5 py-0.5 rounded text-[0.85em] font-mono border border-border">
            {children}
          </code>
        );
      }

      const lang = normalizeLang(match?.[1] ?? "");

      return (
        <div className="relative group my-4 rounded-lg overflow-hidden border border-border">
          {/* copy button top-right, always visible on hover */}
          <div className="absolute top-2 right-2 z-10 opacity-0 group-hover:opacity-100 transition-opacity">
            <CopyCode code={rawCode} />
          </div>
          {mounted ? (
            <SyntaxHighlighter
              style={isDark ? darkStyle : lightStyle}
              language={lang || "text"}
              PreTag="div"
              customStyle={{
                margin: 0,
                borderRadius: 0,
                fontSize: "0.85rem",
                padding: "1.1rem 1rem",
                background: isDark ? "#282b2e" : "#f8f8ff",
                // hide scrollbar
                msOverflowStyle: "none",
                scrollbarWidth: "none",
              }}
            >
              {rawCode}
            </SyntaxHighlighter>
          ) : (
            <pre className="bg-muted px-4 py-4 text-sm font-mono overflow-x-auto [&::-webkit-scrollbar]:hidden">
              <code>{rawCode}</code>
            </pre>
          )}
        </div>
      );
    },

    img({ src, alt }) {
      if (!src) return null;
      return (
        // eslint-disable-next-line @next/next/no-img-element
        <img src={src} alt={alt ?? ""} className="max-w-full rounded-md my-3" loading="lazy"
          onError={(e) => { (e.target as HTMLImageElement).style.display = "none"; }} />
      );
    },

    table({ children }) {
      return (
        <div className="overflow-x-auto my-4 rounded-lg border border-border [&::-webkit-scrollbar]:hidden [-ms-overflow-style:none] [scrollbar-width:none]">
          <table className="w-full text-sm border-collapse">{children}</table>
        </div>
      );
    },
    thead({ children }) { return <thead className="bg-muted">{children}</thead>; },
    th({ children }) { return <th className="border border-border px-3 py-2 text-left font-semibold text-foreground">{children}</th>; },
    td({ children }) { return <td className="border border-border px-3 py-2 text-foreground">{children}</td>; },

    a({ href, children }) {
      const isExternal = href?.startsWith("http");
      return <a href={href} target={isExternal ? "_blank" : undefined} rel={isExternal ? "noopener noreferrer" : undefined} className="text-primary hover:underline break-words">{children}</a>;
    },

    h1({ children }) { return <h1 className="text-[1.6rem] font-bold mt-6 mb-3 pb-2 border-b border-border text-foreground">{children}</h1>; },
    h2({ children }) { return <h2 className="text-[1.3rem] font-bold mt-6 mb-3 pb-2 border-b border-border text-foreground">{children}</h2>; },
    h3({ children }) { return <h3 className="text-[1.1rem] font-semibold mt-5 mb-2 text-foreground">{children}</h3>; },
    h4({ children }) { return <h4 className="text-base font-semibold mt-4 mb-1 text-foreground">{children}</h4>; },
    h5({ children }) { return <h5 className="text-sm font-semibold mt-3 mb-1 text-foreground">{children}</h5>; },
    h6({ children }) { return <h6 className="text-sm font-semibold mt-3 mb-1 text-muted-foreground">{children}</h6>; },

    blockquote({ children }) {
      return <blockquote className="border-l-4 border-muted-foreground/30 pl-4 my-4 text-muted-foreground">{children}</blockquote>;
    },

    ul({ children }) { return <ul className="list-disc pl-6 my-3 space-y-1 text-foreground">{children}</ul>; },
    ol({ children }) { return <ol className="list-decimal pl-6 my-3 space-y-1 text-foreground">{children}</ol>; },
    li({ children }) { return <li className="leading-relaxed text-foreground">{children}</li>; },
    p({ children }) { return <p className="my-3 leading-7 text-foreground">{children}</p>; },
    hr() { return <hr className="my-6 border-border" />; },
    strong({ children }) { return <strong className="font-semibold text-foreground">{children}</strong>; },
    em({ children }) { return <em className="italic text-foreground">{children}</em>; },
    pre({ children }) { return <>{children}</>; },
  };

  return (
    <div className="text-sm leading-relaxed w-full [&_pre_div::-webkit-scrollbar]:hidden [&_pre_div]:scrollbar-none">
      <ReactMarkdown
        remarkPlugins={[remarkGfm]}
        rehypePlugins={[rehypeRaw, [rehypeSanitize, sanitizeSchema]]}
        components={components}
      >
        {fixedContent}
      </ReactMarkdown>
    </div>
  );
}
