# Security policy

## What this library is

A DSP library for a microcontroller, taking audio samples and parameter values
from its application. It opens no network connections. A defect can corrupt
memory or disrupt audio; its impact depends on the firmware embedding it.

Areas covered by this policy include:

- [`test/render.py`](test/render.py) and
  [`demo/render_demo.cpp`](demo/render_demo.cpp), which tests and implements the
  desktop WAV decoder, respectively. The Python test runs the C++ decoder under
  ASan and UBSan against malformed input.
- The delay-memory placement and the staging transport, where a defect is a wild
  write inside a firmware image.

Report those the same way as anything else.

## Supported versions

| Version | Supported |
|---|---|
| The latest release | Yes |
| Earlier releases | No, upgrade to the latest |
| `main` | Yes, as the next release |

While the library is at 0.x there is one supported release at a time: fixes go
on top of the newest one rather than back to older ones.

## Reporting

Report privately through GitHub: [**open a draft security
advisory**](https://github.com/benjaminvdb/cloudseed-daisy/security/advisories/new).
Please do not open a public issue, discussion or pull request for a
vulnerability first.

The private form requires the maintainer to enable private vulnerability
reporting. If it is unavailable, open an issue asking for that feature to be
enabled, without including vulnerability details.

Include what you would put in a bug report (the version or commit, the libDaisy
commit, the toolchain, the build options) plus the input or the sequence that
triggers it, and what you observed.

This is a one-maintainer project. Expect an acknowledgement within two weeks. If
a report is valid, the fix and an advisory go out together, and you will be
credited unless you would rather not be.
