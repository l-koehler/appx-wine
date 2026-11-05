# Toolchain for building the WinRT/XAML shim and running packaged apps.
# nixpkgs is pinned to a 26.05 snapshot.
{
  pkgs ? import (builtins.fetchTarball {
    url = "https://releases.nixos.org/nixos/26.05/nixos-26.05.8409.f4f698677b11/nixexprs.tar.xz";
    sha256 = "sha256-tbu/95O8ZRmCj0kfSiDvz3YT7GSnD0fO6cxLRVYtXnU=";
  }) { },
}:
pkgs.mkShell {
  buildInputs = with pkgs; [
    wineWow64Packages.stable
    pkgsCross.mingw32.buildPackages.gcc
    _7zz
    curl
    unzip
    (python3.withPackages (p: [ p.dnfile ]))
  ];
}
