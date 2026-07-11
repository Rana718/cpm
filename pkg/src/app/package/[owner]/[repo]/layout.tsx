import type { Metadata } from "next";

export async function generateMetadata({
  params,
}: {
  params: Promise<{ owner: string; repo: string }>;
}): Promise<Metadata> {
  const { owner, repo } = await params;
  const title = `${owner}/${repo}`;
  const description = `Install ${repo} with cpm — C/C++ package manager. Get the cpm add command and cpm.toml snippet for ${owner}/${repo}.`;

  return {
    title,
    description,
    openGraph: {
      title: `${title} | cpm registry`,
      description,
      images: [{ url: "/icon.png", width: 500, height: 500, alt: "cpm" }],
    },
    twitter: {
      card: "summary",
      title: `${title} | cpm registry`,
      description,
      images: ["/icon.png"],
    },
  };
}

export default function PackageLayout({ children }: { children: React.ReactNode }) {
  return <>{children}</>;
}
