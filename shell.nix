{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [
    cmake
    gcc
    gnumake
    ninja
    pkg-config
    libusb1
    pcsclite
    python3
  ];
}
