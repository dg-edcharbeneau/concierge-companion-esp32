# Images

| File | What it is | Used by |
|---|---|---|
| `brand/deepgram-wordmark.svg` | Deepgram's 2026 wordmark, `fill="currentColor"`. The source for every inlined copy: the portal, the site's nav, the favicon's D, and `readme-header.svg`. | Nothing directly; copy its paths. |
| `readme-header.svg` | The wordmark in Off-black on the brand's Fuchsia-to-Off-white fade, nothing else beside it (the kit forbids lockups). | The top of `README.md`. |
| `device-orb.gif` | The board speaking, its orb in the default Fuchsia, under the 2026 wordmark. 960×780, 156 frames, 10.4 s, 3.3 MB: over the 1 MB guideline below, accepted for the one animation on the front page. | The README, and the site's hero. |
| `device-orb.png` | The GIF's first frame, exported losslessly. | The site's social card (`og:image`), and the hero for anyone with reduced motion set. |
| `portal.png` | The captive-portal setup page, as a phone sees it after joining the device's AP. | The `Get one running` section of `docs/index.html`. |
| `orb-states/` | Every orb state from the display test, three frames each about 1 s apart, captured from the device's canvas with `CONFIG_UI_SCREENSHOT_TOUR` and `host/shot2png.py`. `contact.png` is all of them on one sheet, a row per state. | The display test section of `docs/display.md`. |

`portal.png` is a real device screenshot rather than the `host/portal.sh`
preview, which is why it shows Android's "Sign in to dg-agent-B1A1" sheet around
the page. Both password fields are empty in it on purpose — the page is
photographed before anything is typed, so nothing in this directory contains a
credential. Keep it that way if you retake either shot.

`host/portal.sh` renders the page with no device attached if you only need the
layout.

Keep files under ~1 MB each; a repo that clones slowly because of screenshots is
its own kind of unfinished.
