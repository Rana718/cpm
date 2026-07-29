{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  # Extra packages for the countries-api project.
  packages = with pkgs; [
    openssl
    postgresql
    spdlog
    libunistring
  ];

  shellHook = ''
    echo "[nix] countries-api dev shell ready"
  '';
}
