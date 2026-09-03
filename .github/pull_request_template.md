## Purpose

<!-- What this changes and why. Link related issues, e.g. "Closes #123". -->

## Test Plan

<!-- How it was tested: the suite on two hosts, which backends, benchmarks run. -->

## Test Result

<!-- Paste the outcome: the suite summary, sanitizer output, before/after benchmark medians. -->

## Checklist

- [ ] `make format` leaves no changes
- [ ] The e2e suite passes on two hosts for both `tcp` and `rdma` (`salloc -N2 scripts/run-e2e.sh`)
- [ ] Clean under `-DRAIL_SANITIZE=address,undefined`
- [ ] Throughput is at or above `main` on the paths touched (`make bench`), with the numbers in Test Result
- [ ] The kernel module still builds if `src/linux` changed
- [ ] Docs updated if behaviour, flags or mount options changed
