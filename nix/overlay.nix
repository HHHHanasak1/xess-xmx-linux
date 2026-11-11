final: prev:
let
  inherit (prev) lib;
  patchDir = ../patches/mesa-26.3;
  patchNames = lib.sort builtins.lessThan
    (builtins.filter (name: lib.hasPrefix "0" name && lib.hasSuffix ".patch" name)
      (builtins.attrNames (builtins.readDir patchDir)));
in {
  mesa = assert lib.assertMsg (prev.mesa.version == "26.3.0")
    "The DG2 fork patch stack is validated against Mesa 26.3.0; rebase it before using another version.";
    prev.mesa.overrideAttrs (old: {
      patches = (old.patches or []) ++ map (name: patchDir + "/${name}") patchNames;
    });
}
