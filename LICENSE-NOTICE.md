# License notice

sloopy-sloop-32 is a derivative port of **SLOOP** (<https://github.com/isod89/sloop-fm1>, by
isod89), which is based on **Felucca** (<https://github.com/hugelton/Felucca>, Copyright (C) 2026
Leo Kuroshita, Hügelton Instruments).

- The port's own code (every file with an `SPDX-License-Identifier: GPL-3.0-only` header) is
  licensed under the GNU General Public License, version 3 only: [LICENSE](LICENSE).
- SLOOP's sources are not copied into this repository: `scripts/fetch-sloop.sh` clones them into
  `upstream/sloop-fm1/` at the commit recorded in `config/upstream.lock`, and the build compiles
  them unmodified. Their licence is GPL-3.0-only (`upstream/SLOOP-LICENSE.txt`, upstream
  `LICENSE`).
- SLOOP's assets (icons, fonts, samples) keep the terms given in upstream `LICENSING.md`: CC0
  samples, the Terminus font (SIL OFL 1.1), the Fukiai icon font (MIT), the Hügelton icon atlas
  under the GPL section 7 permission stated there.
- A firmware image built from this repository contains SLOOP: whoever distributes such an image
  must also offer its complete corresponding source under GPL-3.0-only (this repository plus the
  upstream commit it was built with).

"M-VAVE" and "FM-1" are trademarks of their respective owners. "Felucca" and "Hügelton
Instruments" are names of Hügelton Instruments. This project is not an official M-VAVE, SLOOP or
Hügelton project and is not endorsed by them.
