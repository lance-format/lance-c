# vcpkg overlay port for lance-c

This directory is the canonical copy of the vcpkg port that ships in the
upstream `microsoft/vcpkg` registry. Use it as an overlay against any vcpkg
checkout to install lance-c locally:

```bash
vcpkg install lance-c --overlay-ports=path/to/lance-c/ports
```

After each release, the contents are mirrored into `microsoft/vcpkg` via PR.
