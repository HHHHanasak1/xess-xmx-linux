{
  description = "Mesa 26.3 with XeSS XMX and adaptive DG2 compute register allocation";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/master";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; overlays = [ self.overlays.default ]; };
      mesaIntel = (pkgs.mesa.override {
        galliumDrivers = [ "iris" ];
        vulkanDrivers = [ "intel" ];
      }).overrideAttrs (old: {
        mesonFlags = old.mesonFlags ++ [ "-Dgallium-va=disabled" ];
      });
    in {
      overlays.default = import ./nix/overlay.nix;

      packages.${system} = {
        default = mesaIntel;
        mesa-intel = mesaIntel;
        mesa = pkgs.mesa;
      };

      checks.${system} = {
        patch-series = pkgs.stdenvNoCC.mkDerivation {
          pname = "mesa-dg2-patch-check";
          version = pkgs.mesa.version;
          inherit (pkgs.mesa) src patches;
          dontConfigure = true;
          dontBuild = true;
          installPhase = "touch $out";
        };
        shader-harness = pkgs.stdenv.mkDerivation {
          pname = "dg2-large-grf-harness";
          version = "1";
          src = ./tools/tests/dg2-large-grf;
          nativeBuildInputs = with pkgs; [ pkg-config glslang spirv-tools python3 ];
          buildInputs = with pkgs; [ vulkan-headers vulkan-loader ];
          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            DG2_TEST_BUILD_DIR="$TMPDIR/harness" bash ./run.sh --build-only
            runHook postBuild
          '';
          installPhase = ''
            mkdir -p "$out/bin" "$out/share/dg2-large-grf"
            cp "$TMPDIR/harness/execute" "$out/bin/dg2-large-grf-test"
            cp "$TMPDIR/harness/pressure.spv" "$out/share/dg2-large-grf/"
          '';
        };
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          gcc pkg-config glslang spirv-tools python3 vulkan-headers vulkan-loader
          vulkan-tools
        ];
      };
    };
}
