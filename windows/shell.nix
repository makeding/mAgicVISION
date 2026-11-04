{ pkgs ? import <nixpkgs> {} }:
let cross = pkgs.pkgsCross.mingwW64;
in pkgs.mkShell {
  packages = [ cross.stdenv.cc pkgs.cmake pkgs.ninja pkgs.python3 pkgs.llvmPackages.clang-unwrapped ];
  CLANG_MSVC = "${pkgs.llvmPackages.clang-unwrapped}/bin/clang++";
  MCFGTHREAD_LIB_DIR = "${cross.windows.mcfgthreads}/lib";
}
