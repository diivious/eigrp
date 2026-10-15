# Security Policy

## Supported Versions

OpenEIGRP is under active pre-production development. Security fixes are focused on the current `master` branch. Published tags and development snapshots do not automatically receive security updates or backported fixes.

| Version | Security Support |
| ------- | ---------------- |
| `master` (current development) | Best effort |
| Previous alpha/beta snapshots and release tags | No routine backports |
| Stable/production releases | Not yet designated |

This policy may change when production releases and a formal maintenance schedule are established.

## Reporting a Vulnerability

**Do not disclose security vulnerabilities in public GitHub issues, pull requests, or discussions.**

Please report suspected vulnerabilities privately using either of these channels:

1. **Preferred:** [GitHub private vulnerability reporting](https://github.com/diivious/eigrp/security/advisories/new), when enabled for the repository.
2. **Alternative:** Email `diivious@hotmail.com` with the subject `OpenEIGRP Security Report`.

Please include, when available:

- A description of the vulnerability and its potential impact.
- The affected commit, branch, release tag, or build.
- The affected environment or host integration (such as FRR, Unix, or BIRD).
- Steps to reproduce and any minimal proof of concept, including crafted packets or relevant logs.
- Any known workarounds or mitigations.

Do not include passwords, private keys, or other sensitive production data in reports.

## Response and Disclosure

The maintainer aims to acknowledge reports within **7 calendar days** and provide status updates during investigation. These are best-effort targets, not guaranteed response or remediation deadlines.

Reports will be reviewed for reproducibility, security impact, and applicability to OpenEIGRP.

Accepted reports may result in a fix, mitigation guidance, and a GitHub security advisory when appropriate. If a report is not accepted as a security vulnerability, the maintainer will aim to explain why.

Please allow time for investigation and remediation before public disclosure, and coordinate any disclosure date with the maintainer.

Security fixes generally target the current development branch; a separate patched release may be published when warranted.

OpenEIGRP's pre-production status should not be interpreted as a guarantee of security or suitability for production deployment.
