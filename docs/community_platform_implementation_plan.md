# Boo Sah Sah / Orchid Commons in Demo-Verse

Boo Sah Sah is Orchid Commons, the founding world of **Demo-Verse**. The platform architecture, world descriptor, reference-room plan and interoperable examples live in the standalone `demo-verse` repository. In the sibling checkout, start with the [Demo-Verse architecture](../../demo-verse/README.md) and [founding world descriptor](../../demo-verse/examples/orchid-commons.json).

## The film's part

- **Watch, Roam, Teleport.** The film provides its scripted camera and the existing `F` Explore toggle. The outbound bridge accepts a public HTTPS world address from either mode. The visitor chooses the installed Demo-Verse visitor or an explicitly labeled browser landing page. A full teleport completes when the destination visitor admits the traveler and renders the new world. Ordinary playback stays local.
- **Inbound visit.** The proposed Orchid Commons atrium offers a film experience through a locally installed, trusted adapter identified by `boo-sah-sah-glass-orchid`. Other visitors can stay together in the atrium. A world file cannot specify an executable, arguments or a download.
- **Scheduled screenings.** The film already accepts `--start-seconds`. The [full cue sheet](../assets/cues/film_360s.json) defines a 360-second show at 48 kHz. A shared show clock, playback feedback, and seek/pause control belong to a future adapter. A browser departure does not create shared presence.
- **Release ownership.** This repository owns the native film, its offline behavior and asset licensing. The Demo-Verse repository owns the visitor, world format, hosts and cross-world tests. Film content can stay with its publisher while independent creators host their own experiences.
