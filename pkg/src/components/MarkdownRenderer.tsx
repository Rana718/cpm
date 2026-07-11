"use client";

import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import rehypeRaw from "rehype-raw";
import rehypeSanitize, { defaultSchema } from "rehype-sanitize";
import { Prism as SyntaxHighlighter } from "react-syntax-highlighter";
import { oneDark } from "react-syntax-highlighter/dist/esm/styles/prism";
import type { Components } from "react-markdown";
import { useState } from "react";
import { AiOutlineCopy, AiOutlineCheck } from "react-icons/ai";

// Allow style, align and other attrs GitHub READMEs use
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

function CopyCode({ code }: { code: string }) {
  const [copied, setCopied] = useState(false);
  return (
    <button
      onClick={async () => {
        await navigator.clipboard.writeText(code);
        setCopied(true);
        setTimeout(() => setCopied(false), 2000);
      }}
      className="absolute top-2 right-2 p-1.5 rounded bg-white/10 hover:bg-white/20 transition-colors text-white/70 hover:text-white"
    >
      {copied ? <AiOutlineCheck className="h-3.5 w-3.5" /> : <AiOutlineCopy className="h-3.5 w-3.5" />}
    </button>
  );
}

interface MarkdownRendererProps {
  content: string;
  // owner/repo needed to resolve relative image paths
  owner: string;
  repo: string;
  branch: string;
}

export function MarkdownRenderer({ content, owner, repo, branch }: MarkdownRendererProps) {
  // Fix relative image paths → absolute raw GitHub URLs
  const fixedContent = content.replace(
    /!\[([^\]]*)\]\((?!https?:\/\/)([^)]+)\)/g,
    (_, alt, src) => {
      const clean = src.replace(/^\.\//, "");
      return `![${alt}](https://raw.githubusercontent.com/${owner}/${repo}/${branch}/${clean})`;
    }
  );

  const components: Components = {
    // Syntax highlighted code blocks
    code({ className, children, ...props }) {
      const match = /language-(\w+)/.exec(className ?? "");
      const code = String(children).replace(/\n$/, "");
      const isInline = !match && !String(children).includes("\n");

      if (isInline) {
        return (
          <code
            className="bg-muted text-foreground px-1.5 py-0.5 rounded text-[0.85em] font-mono border border-border"
            {...props}
          >
            {children}
          </code>
        );
      }

      const lang = match?.[1] ?? "text";
      return (
        <div className="relative group my-4">
          <div className="flex items-center justify-between bg-zinc-800 rounded-t-lg px-4 py-1.5 text-xs text-zinc-400 font-mono">
            <span>{lang}</span>
            <CopyCode code={code} />
          </div>
          <SyntaxHighlighter
            style={oneDark}
            language={lang}
            PreTag="div"
            customStyle={{
              margin: 0,
              borderRadius: "0 0 0.5rem 0.5rem",
              fontSize: "0.85rem",
              border: "none",
            }}
          >
            {code}
          </SyntaxHighlighter>
        </div>
      );
    },

    // Images — use Next img with error fallback
    img({ src, alt, ...props }) {
      if (!src) return null;
      return (
        // eslint-disable-next-line @next/next/no-img-element
        <img
          src={src}
          alt={alt ?? ""}
          className="max-w-full rounded-lg my-2"
          loading="lazy"
          onError={(e) => {
            (e.target as HTMLImageElement).style.display = "none";
          }}
          {...props}
        />
      );
    },

    // Tables
    table({ children }) {
      return (
        <div className="overflow-x-auto my-4 [&::-webkit-scrollbar]:hidden [-ms-overflow-style:none] [scrollbar-width:none]">
          <table className="w-full text-sm border-collapse border border-border">
            {children}
          </table>
        </div>
      );
    },
    th({ children }) {
      return (
        <th className="border border-border bg-muted px-3 py-2 text-left font-semibold text-foreground">
          {children}
        </th>
      );
    },
    td({ children }) {
      return (
        <td className="border border-border px-3 py-2 text-foreground">
          {children}
        </td>
      );
    },

    // Links — open external in new tab
    a({ href, children }) {
      const isExternal = href?.startsWith("http");
      return (
        <a
          href={href}
          target={isExternal ? "_blank" : undefined}
          rel={isExternal ? "noopener noreferrer" : undefined}
          className="text-primary hover:underline"
        >
          {children}
        </a>
      );
    },

    // Headings
    h1({ children }) {
      return <h1 className="text-2xl font-bold mt-6 mb-3 text-foreground border-b border-border pb-2">{children}</h1>;
    },
    h2({ children }) {
      return <h2 className="text-xl font-bold mt-5 mb-2 text-foreground border-b border-border pb-1">{children}</h2>;
    },
    h3({ children }) {
      return <h3 className="text-lg font-semibold mt-4 mb-2 text-foreground">{children}</h3>;
    },
    h4({ children }) {
      return <h4 className="text-base font-semibold mt-3 mb-1 text-foreground">{children}</h4>;
    },

    // Blockquote
    blockquote({ children }) {
      return (
        <blockquote className="border-l-4 border-primary/40 pl-4 my-3 text-muted-foreground italic">
          {children}
        </blockquote>
      );
    },

    // Lists
    ul({ children }) {
      return <ul className="list-disc list-inside my-2 space-y-1 text-foreground pl-2">{children}</ul>;
    },
    ol({ children }) {
      return <ol className="list-decimal list-inside my-2 space-y-1 text-foreground pl-2">{children}</ol>;
    },
    li({ children }) {
      return <li className="text-foreground leading-relaxed">{children}</li>;
    },

    // Paragraph
    p({ children }) {
      return <p className="my-2 leading-relaxed text-foreground">{children}</p>;
    },

    // Horizontal rule
    hr() {
      return <hr className="my-4 border-border" />;
    },
  };

  return (
    <div className="text-sm leading-relaxed">
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
