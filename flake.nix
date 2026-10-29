{
  description = "Development environment for nativeTv";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (
        pkgs:
        let
          # The docs workflow runs on 3.12 and the Home Assistant one on 3.13;
          # the check scripts themselves use only the standard library.
          python = pkgs.python313;

          # mkdocs.yml's callouts extension, not packaged in nixpkgs.
          markdown-callouts = python.pkgs.buildPythonPackage rec {
            pname = "markdown-callouts";
            version = "0.4.0";
            pyproject = true;
            src = python.pkgs.fetchPypi {
              pname = "markdown_callouts";
              inherit version;
              hash = "sha256-ftLJBIaWcFinOlR3gRIZg4OVItZwQa5SxJeWFvGyt0Y=";
            };
            build-system = [ python.pkgs.hatchling ];
            dependencies = [ python.pkgs.markdown ];
          };
        in
        {
          default = pkgs.mkShell {
            packages = [
              # The dashboards workflow's node, which also runs the UI suites and the
              # contract checks; the checks workflow runs on ubuntu-latest's.
              pkgs.nodejs_22

              # The scripts/check-*.py gate and build-ipk.py, plus the docs
              # site pinned as the docs workflow pins it: MkDocs 2 drops the
              # theme and plugin system Material is built on.
              (python.withPackages (ps: [
                ps.mkdocs
                ps.mkdocs-material
                markdown-callouts
              ]))

              # The checks workflow's shell script lint.
              pkgs.shellcheck
            ];
          };
        }
      );
    };
}
