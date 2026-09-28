# Security policy

## Supported versions

| Version | Supported |
|---|---|
| 1.4.x | Yes |
| Earlier prototypes | No |

## Reporting a vulnerability

Please use **Security → Report a vulnerability** on the GitHub repository. This
opens a private vulnerability report visible only to the maintainer and GitHub
security staff. Do not disclose the issue in a public issue, discussion, or pull
request before a coordinated fix is available.

Include affected versions, impact, prerequisites, reproduction steps, and any
suggested mitigation. Reports should receive an initial response within seven
days. Hardware access-point behavior is intentionally local and unauthenticated
except for WPA2; reports should distinguish that design boundary from an actual
escape, memory-safety problem, or cross-client vulnerability.

Game data and copyright reports are not security vulnerabilities. The current
source tree excludes the previously tracked `doom1.whx`, and the inspected
`v1.4.0` firmware archive contains no WAD, WHX, WHD, or generated audio pack.
The file remains in earlier Git history; this removal does not rewrite history
or erase copies in existing clones and forks. For copyright or packaging
concerns, contact the maintainer privately rather than posting a download link
or the asset itself in a public issue.
