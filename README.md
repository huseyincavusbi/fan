# fan

Lightweight fan control for Mac mini. One C++ binary:
CLI + root launchd daemon, no dependencies. Tested on Mac mini M4, macOS 27.0.1.

## Install

```sh
make
sudo ./fan install
```

## Usage

```sh
fan set 3000      # pin RPM
fan auto          # back to the curve / macOS auto
fan status        # fan state and temperatures
```

## Warning

Personal project, shared as-is. It writes to the SMC through an undocumented
Apple interface and runs a root daemon. Bugs or unsupported configurations could
cause overheating or damage, and may affect warranty eligibility if damage is
attributed to its use. Use at your own risk. Not affiliated with Apple.

## License

MIT - Copyright (c) 2026 Huseyin Cavus
