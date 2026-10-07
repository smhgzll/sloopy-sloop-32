# Upstream SLOOP workflow

Fetch/update:

```bash
./scripts/fetch-sloop.sh
```

Pin a specific commit/tag when reproducibility matters:

```bash
SLOOP_REF=<commit-or-tag> ./scripts/fetch-sloop.sh
```

Record the commit used for a known-good port in `config/upstream.lock` once the first baseline is established.

Do not edit `upstream/sloop-fm1` blindly. Prefer port adapters/patches or a clearly documented fork strategy so future upstream changes remain reviewable.
