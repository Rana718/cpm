import type { Metadata } from "next";

export const metadata: Metadata = {
  title: "Documentation",
  description:
    "Learn how to install cpm, create projects, add C/C++ dependencies, and build production binaries with the cpm C/C++ package manager.",
  openGraph: {
    title: "cpm Documentation — C/C++ Package Manager",
    description:
      "Complete guide to using cpm: installation, cpm.toml config, commands, header-only and compiled dependencies, and production builds.",
    images: [{ url: "/icon.png", width: 500, height: 500, alt: "cpm" }],
  },
};

export default function DocsLayout({ children }: { children: React.ReactNode }) {
  return <>{children}</>;
}
