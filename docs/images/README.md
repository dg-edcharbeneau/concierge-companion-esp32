# Images

| File | What it is | Used by |
|---|---|---|
| `brand/deepgram-wordmark.svg` | Deepgram's 2026 wordmark, `fill="currentColor"`. The source for every inlined copy: the portal, the site's nav, the favicon's D, and `readme-header.svg`. | Nothing directly; copy its paths. |
| `readme-header.svg` | The wordmark in Off-black on the brand's Fuchsia-to-Off-white fade, nothing else beside it (the kit forbids lockups). | The top of `README.md`. |
| `device-orb.gif` | The board speaking, its orb in the default Fuchsia, under the 2026 wordmark. 960×780, 156 frames, 10.4 s, 3.3 MB: over the 1 MB guideline below, accepted for the one animation on the front page. | The README, and the site's hero. |
| `device-orb.png` | The GIF's first frame, exported losslessly. | The site's social card (`og:image`), and the hero for anyone with reduced motion set. |
| `portal.png` | The captive-portal setup page at phone width, from `host/portal.sh`. | The `Get one running` section of `docs/index.html`. |
| `orb-states/` | Every orb state from the display test, three frames each about 1 s apart, captured from the device's canvas with `CONFIG_UI_SCREENSHOT_TOUR` and `host/shot2png.py`. `contact.png` is all of them on one sheet, a row per state. | The display test section of `docs/display.md`. |

`portal.png` is the `host/portal.sh` preview at phone width, not a device
screenshot: "kitchen-2.4" is the preview's made-up network, and there is no
Android sign-in sheet around it. It shows the 2026 page exactly as the firmware
embeds it, only without a device behind it. Both password fields are empty in it
on purpose, so nothing in this directory contains a credential. Keep it that way
if you retake it, from the preview or from a phone.

`host/portal.sh` renders the page with no device attached if you only need the
layout.

Keep files under ~1 MB each; a repo that clones slowly because of screenshots is
its own kind of unfinished.
