{
  inputs = {
    # Keep this revision identical to the Bazel git_override in a published
    # consumer. Repository tests override this input with the local checkout.
    zub_1cg.url = "git+ssh://git@10.10.0.101/v/zub_1cg.git?rev=85a35fdb7edaf79f67dadb1252dcbbf636bea588";
  };
  outputs = { zub_1cg, ... }: {
    devShells.x86_64-linux.default = zub_1cg.lib.mkDevShell {
      system = "x86_64-linux";
      extraShellHook = ''echo "zub_1cg consumer shell"'';
    };
  };
}
