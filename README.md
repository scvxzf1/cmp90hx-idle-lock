# cmp90hx-idle-lock

A small Linux systemd daemon that lowers an NVIDIA CMP 90HX memory clock to
405 MHz after 60 seconds of GPU inactivity and removes the clock lock as soon
as activity returns.

The installer checks for compatible hardware, while the daemon enumerates every
NVML device at startup and creates an independent state machine for each CMP
90HX it finds. Each card is pinned by its UUID, PCI address, subsystem ID,
device ID, model name, and VRAM size. The daemon never selects a GPU by its
mutable index. If no compatible card is present at boot, the systemd
`ExecCondition` skips the long-running process.

## Behavior

- Samples NVML telemetry once per second.
- Requires stable zero GPU and memory-controller utilization, VRAM use, and
  process set for 60 seconds before locking memory to 405 MHz.
- Removes the memory clock lock on activity, telemetry failure, identity
  mismatch, or clean shutdown.
- Detects and reapplies an idle lock changed by another GPU management tool.
- Supports multiple CMP 90HX cards simultaneously; one card failing identity or
  telemetry checks does not affect the others.
- Does not open network sockets or control core clocks, power limits, or any
  other GPU.
- Runs as root because NVIDIA restricts clock control to privileged processes.

## Requirements

- Linux with systemd
- NVIDIA driver with `libnvidia-ml.so`
- `nvidia-smi`, a C compiler, and `make`
- One NVIDIA CMP 90HX with device ID `10de:220d` and 10 GiB VRAM

The NVML declarations match the ABI used by current NVIDIA Linux drivers. The
daemon has been tested with driver 580.159.03 on x86-64.

## Build and install

```sh
make
pkexec ./install.sh
```

The installer verifies that at least one compatible CMP 90HX is present, installs
the binary under `/usr/local/sbin`, and enables and starts the service. Device
identifiers are discovered dynamically at every daemon start; no machine UUID
is stored in the repository or required in a configuration file.

## Inspect

```sh
systemctl status cmp90hx-idle-lock
journalctl -u cmp90hx-idle-lock -f
```

## Uninstall

```sh
sudo systemctl disable --now cmp90hx-idle-lock
sudo rm -f /etc/systemd/system/cmp90hx-idle-lock.service
sudo rm -f /etc/default/cmp90hx-idle-lock
sudo rm -f /usr/local/sbin/cmp90hx-idle-lock
sudo systemctl daemon-reload
```

## License

MIT
