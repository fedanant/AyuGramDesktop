# Updating tg-ws-proxy

`Telegram/ThirdParty/tg-ws-proxy` is pinned to an upstream commit. AyuGram's
adapter and Windows packaging live in `Telegram/build/tg_ws_proxy/` so upstream
updates do not overwrite them.

## Automatic update PRs

Dependabot checks the upstream `main` branch every Monday at 10:00 Moscow time
and opens at most one update PR targeting AyuGram's `main` branch. Its allowlist
includes only this submodule. Updates track commits, not just release tags.

GitHub reads `.github/dependabot.yml` from the repository's default branch,
which is currently `dev` in `fedanant/AyuGramDesktop`. Install that configuration
on `dev` as well as `main` to activate the schedule; `target-branch: main` controls
where the update PRs go. The `TG WS Proxy` workflow must be present in `main`.
See [GitHub's Dependabot configuration documentation](https://docs.github.com/en/code-security/reference/supply-chain-security/dependabot-options-reference).

The `TG WS Proxy / Windows packaged proxy` check runs on relevant PRs and pushes
to `main`. It builds the standalone helper using the production build script,
runs the upstream unit tests, then runs the resulting executable from a
temporary directory with spaces in its path. It checks both Cloudflare fallback
settings, repeated CLI options, TCP startup, invalid-handshake rejection, log
redaction, and shutdown when the supplied parent process exits. DC targets are
loopback addresses and custom domains use `.invalid`; no Telegram account or
release secrets are required. This is a compatibility smoke test, not a live
test of Telegram connectivity or Cloudflare routing.

Review upstream changes and the check before merging. In particular, changes
to `proxy.config`, `_run`, or Python dependencies may require changes to the
adapter or the dependencies pinned in `build.py`. Auto-merge is not configured.
If branch protection should enforce this check, configure it separately; with
the workflow's path filters, it should not be required for unrelated PRs.

## Manual update and verification

From the repository root, update the tracked branch and review its commits:

```powershell
git submodule update --init --remote -- Telegram/ThirdParty/tg-ws-proxy
git diff --submodule=log -- Telegram/ThirdParty/tg-ws-proxy
```

To select a release instead, fetch tags in the submodule and check out the
chosen tag with `git checkout --detach <tag>` before testing.

On Windows with Python 3.10, run the same build and smoke test as CI:

```powershell
python Telegram/build/tg_ws_proxy/build.py --source Telegram/ThirdParty/tg-ws-proxy --build out/tg-ws-proxy-check/build --output out/tg-ws-proxy-check/AyuWsProxy.exe --runner Telegram/build/tg_ws_proxy/runner.py
python .github/scripts/tg_ws_proxy_smoke.py --executable out/tg-ws-proxy-check/AyuWsProxy.exe
```

After review and verification, commit the new submodule pointer:

```powershell
git add Telegram/ThirdParty/tg-ws-proxy
git commit -m "Update tg-ws-proxy"
```

Normal builds and CI use `git submodule update --init` to check out the pinned
commit. Keep `--remote` in the deliberate update step, not in the build workflow.
To roll back a merged update, revert its commit in a new PR and rerun the check.
