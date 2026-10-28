# Redactions

`HLS000000.TXT` is the wrapper log with identifiers removed. Every other byte is as written at boot.

- The NVMe namespace EUI-64 in device-path nodes `0317...` is replaced with `x`.
- The partition signatures (GPT GUID / MBR signature) in hard-drive nodes `0401...` are replaced with `x`.

Original sha256 of the unredacted log: `aa4dc9feccaf7ef77c129c3288ea9b03393a789af8080f84309d0bf055822126`.
`PREST02.TXT` and `AUTO000000-REPORT.TXT` are unredacted. `SHA256SUMS` lists the files as published.
