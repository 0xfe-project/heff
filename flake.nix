{
  description = "heff, a functional language with a phase distinction";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      # (no x86_64-darwin: the nixpkgs that has zig 0.17 has dropped it)
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      # (the version the package says, `heff = "X.Y.Z"`)
      version = builtins.head (
        builtins.match ''.*heff = "([0-9.]+)".*'' (builtins.readFile ./package.hf)
      );
    in
    {
      packages = forAll (
        pkgs:
        let
          heff = pkgs.stdenv.mkDerivation {
            pname = "heff";
            inherit version;
            src = self;

            nativeBuildInputs = [
              pkgs.zig_0_17
              pkgs.makeWrapper
            ]
            ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isLinux pkgs.autoPatchelfHook;

            # (the build from the seed: zig alone, nothing fetched; zig's
            # caches where the build may write)
            dontConfigure = true;
            buildPhase = ''
              runHook preBuild
              export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-global-cache
              export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
              zig build
              runHook postBuild
            '';

            # (laid out as a release is: the program in bin/, its library
            # and package beside bin/, where it looks for them; zig, which
            # `heff build` runs, found after any on the PATH)
            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp .heff/out/heff $out/bin/heff
              cp -r LICENSE README.md docs lib package.hf $out/
              wrapProgram $out/bin/heff --suffix PATH : ${pkgs.lib.makeBinPath [ pkgs.zig_0_17 ]}
              runHook postInstall
            '';

            meta = {
              description = "A functional language with a phase distinction";
              homepage = "https://github.com/dannypsnl/heff";
              license = pkgs.lib.licenses.bsd3;
              mainProgram = "heff";
              platforms = systems;
            };
          };
        in
        {
          inherit heff;
          default = heff;
        }
      );

      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.heff}/bin/heff";
          meta.description = "heff";
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.zig_0_17
            pkgs.gnumake
            pkgs.nodejs
            pkgs.git
            pkgs.xz
          ];
        };
      });
    };
}
