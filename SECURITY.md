# Security Policy

## Supported versions

Only the latest release receives security fixes.

## Reporting a vulnerability

Please do not open a public issue for security problems. Instead:

- Report privately via GitHub:
  [Security → Report a vulnerability](https://github.com/crazyguitar/rail/security/advisories/new), or
- Email the maintainer: spiderpower02@gmail.com

Include the rail version, your environment (OS and kernel on both hosts,
rdma-core, NIC), which path was in use, and reproduction steps. You should
get a response within a week.

## Scope notes

rail moves bytes between machines and exposes a directory to the kernel, to
FUSE and over NFS, and its daemon decodes frames sent by its peer. Reports of
a path escaping the export root, of a malformed frame crashing or hanging the
daemon or the kernel module, of one client's data reaching another, or of a
peer being able to write outside the page it was offered are very welcome.
