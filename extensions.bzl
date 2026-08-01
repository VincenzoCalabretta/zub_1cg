"""Module extensions: Eclipse ThreadX / FileX / NetX Duo source fetchers.

Each extension downloads an upstream release tarball and overlays a BUILD
file from //third_party/... onto the extracted source tree.
"""

# ── ThreadX ──────────────────────────────────────────────────────────────────

def _threadx_repo_impl(repo_ctx):
    repo_ctx.download_and_extract(
        url = "https://github.com/eclipse-threadx/threadx/archive/refs/tags/v6.4.3.202503_rel.tar.gz",
        stripPrefix = "threadx-6.4.3.202503_rel",
    )
    repo_ctx.file(
        "BUILD.bazel",
        repo_ctx.read(Label("//third_party/threadx:BUILD.bazel")),
    )

_threadx_repo = repository_rule(implementation = _threadx_repo_impl)

def _threadx_extension_impl(module_ctx):
    _threadx_repo(name = "threadx")

threadx_extension = module_extension(implementation = _threadx_extension_impl)

# ── Eclipse FileX ────────────────────────────────────────────────────────────

def _filex_repo_impl(repo_ctx):
    repo_ctx.download_and_extract(
        url = "https://github.com/eclipse-threadx/filex/archive/refs/tags/v6.5.1.202602_rel.tar.gz",
        stripPrefix = "filex-6.5.1.202602_rel",
    )
    repo_ctx.file(
        "BUILD.bazel",
        repo_ctx.read(Label("//third_party/filex:BUILD.bazel")),
    )

_filex_repo = repository_rule(implementation = _filex_repo_impl)

def _filex_extension_impl(module_ctx):
    _filex_repo(name = "filex")

filex_extension = module_extension(implementation = _filex_extension_impl)

# ── Eclipse NetX Duo ─────────────────────────────────────────────────────────

def _netxduo_repo_impl(repo_ctx):
    repo_ctx.download_and_extract(
        url = "https://github.com/eclipse-threadx/netxduo/archive/refs/tags/v6.5.1.202602_rel.tar.gz",
        stripPrefix = "netxduo-6.5.1.202602_rel",
    )
    repo_ctx.file(
        "BUILD.bazel",
        repo_ctx.read(Label("//third_party/netxduo:BUILD.bazel")),
    )

_netxduo_repo = repository_rule(implementation = _netxduo_repo_impl)

def _netxduo_extension_impl(module_ctx):
    _netxduo_repo(name = "netxduo")

netxduo_extension = module_extension(implementation = _netxduo_extension_impl)
