import type { Metadata } from "next";
import { Geist, Geist_Mono, Noto_Sans, Playfair_Display } from "next/font/google";
import "./globals.css";
import { cn } from "@/lib/utils";
import { Providers } from "./providers";
import { Navbar } from "@/components/Navbar";

const playfairDisplayHeading = Playfair_Display({ subsets: ["latin"], variable: "--font-heading" });
const notoSans = Noto_Sans({ subsets: ["latin"], variable: "--font-sans" });
const geistSans = Geist({ variable: "--font-geist-sans", subsets: ["latin"] });
const geistMono = Geist_Mono({ variable: "--font-geist-mono", subsets: ["latin"] });

const BASE_URL = "https://cpm-registry.vercel.app";

export const metadata: Metadata = {
  metadataBase: new URL(BASE_URL),
  title: {
    default: "cpm registry — Find C/C++ Packages",
    template: "%s | cpm registry",
  },
  description:
    "Search any C/C++ library on GitHub and get the exact cpm add command and cpm.toml snippet — ready to copy. The package registry for the cpm C/C++ package manager.",
  keywords: [
    "C++ package manager", "C package manager", "cpm", "cpp packages",
    "C++ libraries", "header-only", "cmake", "conan alternative",
    "vcpkg alternative", "nlohmann json", "fmt", "boost", "spdlog",
    "grpc", "protobuf", "openssl", "C++ dependency manager",
  ],
  authors: [{ name: "Rana718", url: "https://github.com/Rana718" }],
  creator: "Rana718",
  publisher: "cpm",
  robots: {
    index: true,
    follow: true,
    googleBot: { index: true, follow: true },
  },
  openGraph: {
    type: "website",
    url: BASE_URL,
    title: "cpm registry — Find C/C++ Packages",
    description:
      "Search any C/C++ library on GitHub and get the exact cpm add command and cpm.toml snippet ready to copy.",
    siteName: "cpm registry",
    images: [
      {
        url: "/icon.png",
        width: 500,
        height: 500,
        alt: "cpm — C/C++ Package Manager",
      },
    ],
  },
  twitter: {
    card: "summary",
    title: "cpm registry — Find C/C++ Packages",
    description:
      "Search any C/C++ library on GitHub and get the exact cpm add command ready to copy.",
    images: ["/icon.png"],
  },
  icons: {
    icon: [
      { url: "/icon.png", type: "image/png" },
    ],
    apple: "/icon.png",
    shortcut: "/icon.png",
  },
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html
      lang="en"
      suppressHydrationWarning
      className={cn(
        "h-full antialiased",
        geistSans.variable,
        geistMono.variable,
        "font-sans",
        notoSans.variable,
        playfairDisplayHeading.variable
      )}
    >
      <body className="min-h-full flex flex-col overflow-x-hidden">
        <Providers>
          <Navbar />
          <div className="flex flex-col flex-1">
            {children}
          </div>
        </Providers>
      </body>
    </html>
  );
}
