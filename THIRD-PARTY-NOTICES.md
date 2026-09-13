# Third-party notices

This library is MIT licensed ([LICENSE](LICENSE)). It contains work by others,
under their own terms, listed here. The full license texts are reproduced in
[`src/cloudseed/license.txt`](src/cloudseed/license.txt), and every file that
carries a notice of its own keeps it.

*(These notices lived at the foot of `LICENSE` until they were moved here:
GitHub's license detector matches the whole of `LICENSE` against the licenses it
knows, and the appended text made it report this repository as "Other" rather
than MIT. GitHub's own guidance is to keep `LICENSE` simple and note the
complexity elsewhere.)*

| Component | Where | Copyright | License |
|---|---|---|---|
| Cloud Seed, the reverb kernel this library ports | [`src/cloudseed/`](src/cloudseed/) | © 2018 Valdemar Erlingsson | MIT |
| Cloud Seed 2's core, the Dark Plate program | [`src/cloudseed/presets.h`](src/cloudseed/presets.h) | © 2024 Ghost Note Engineering Ltd | MIT |
| SHA-256 | [`src/cloudseed/sha_random.cpp`](src/cloudseed/sha_random.cpp) | Olivier Gay | Modified BSD |
| fdlibm argument reduction | [`src/cloudseed/fdlibm_trig.cpp`](src/cloudseed/fdlibm_trig.cpp), [`test/trig.cpp`](test/trig.cpp) | © 1993 Sun Microsystems, Inc. | fdlibm notice |

## Cloud Seed

The reverb kernel in `src/cloudseed` is a port of [Cloud
Seed](https://github.com/ValdemarOrn/CloudSeed) by Valdemar Erlingsson, released
under the MIT License, Copyright (c) 2018 Valdemar Erlingsson. Its factory
programs in `src/cloudseed/presets.h` are the plugin's.

## Cloud Seed 2

The Dark Plate program in `src/cloudseed/presets.h` is adapted from the built-in
program of [Cloud Seed 2's
core](https://github.com/GhostNoteAudio/CloudSeedCore) by Ghost Note Engineering
Ltd, released under the MIT License, Copyright (c) 2024 Ghost Note Engineering
Ltd.

## SHA-256

The SHA-256 implementation in `src/cloudseed/sha_random.cpp` is by Olivier Gay,
under the Modified BSD License. The notice is in the file.

## fdlibm

The argument reduction in `src/cloudseed/fdlibm_trig.cpp` and its reference in
`test/trig.cpp` are derived from fdlibm, Copyright (C) 1993 by Sun Microsystems,
Inc.:

> Permission to use, copy, modify, and distribute this software is freely
> granted, provided that this notice is preserved.

## The demo sources

The dry phrases the demo clips are built from ([`demo/dry/`](demo/dry/)) are
CC0; [`demo/README.md`](demo/README.md) names each one and where it came from.

## The example firmware binary

The release image also links libDaisy, its STM32 and CMSIS dependencies, and the
Arm toolchain's runtime libraries. The release includes
`THIRD-PARTY-NOTICES.txt`, assembled from those build inputs by
[`scripts/release-notices.sh`](scripts/release-notices.sh). Keep that file with
the firmware when redistributing it. These dependencies are not included in this
library's source archive.
