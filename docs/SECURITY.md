# Security and secrets

- `config/local.env` is untracked.
- `config/sdkconfig.local.defaults` is generated and untracked.
- Do not put real Wi-Fi passwords in `sdkconfig.defaults`, source files, screenshots or test fixtures.
- Prefer provisioning/runtime configuration later if the device becomes a distributable product.
- Development AP defaults must not be reused as production credentials.
