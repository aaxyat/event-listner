# Docker Event Listener (`event-listner`)

An ultra-lightweight, extensible Docker container lifecycle and health event listener written in native C.

Connects directly to the Docker UNIX domain socket (`/var/run/docker.sock`), streaming events via kernel `epoll_wait()` and triggering customized recovery scripts or hooks whenever any container enters an `unhealthy`, `die`, `oom`, or `restart` state.

---

## Performance & Memory

Standard listeners using `docker events` CLI and shell scripts carry heavy runtime overhead:

| Implementation | Steady-State RAM | OS Tasks / Threads | Idle CPU |
| :--- | :--- | :--- | :--- |
| `docker events` CLI + `bash` pipe | **~13.5 MB** | 9 threads (Go runtime + shells) | 0% |
| **`docker-event-listener` (Native C)** | **~176 KB** | **1 thread** | **0.00%** |

This represents a **98.7% reduction** in memory footprint while providing instant, sub-second failure reaction time.

---

## Extensible Handler Architecture

`docker-event-listener` supports multiple dispatch methods for all your present and future containers:

### 1. Configuration Rules File (`events.conf`)
Map container events directly to shell commands or scripts in `/docker/config/events.conf`:
```ini
# <container_pattern>:<event_pattern> = <command>

# VPN Auto-healing
gluetun:unhealthy = /docker/scripts/gluetun-watchdog.sh

# Torrent / download clients
qbittorrent:die = docker compose -f /docker/compose/qbittorrent/docker-compose.yml restart qbittorrent

# Media servers
audiobookshelf:unhealthy = docker compose -f /docker/compose/audiobookshelf/docker-compose.yml restart audiobookshelf

# Global Out-Of-Memory alert
*:oom = echo "OOM on container $CONTAINER_NAME" | wall
```
*Reload rules without restarting using `kill -HUP <pid>` or `systemctl reload gluetun-event-listener`.*

### 2. Dynamic Hooks Directory (`events.d/`)
Drop executable scripts into `/docker/scripts/events.d/` named:
- `<container>_<event>.sh` (e.g. `gluetun_unhealthy.sh`)
- `<container>.sh` (e.g. `gluetun.sh`)
- `default.sh` (global catch-all)

Environment variables passed to each hook:
- `$CONTAINER_NAME` — name of the container
- `$EVENT_ACTION` — raw event string (e.g. `health_status: unhealthy`, `die`, `oom`)
- `$CONTAINER_ID` — 64-character container ID

---

## Building & Installation

### Requirements
- GCC or Clang
- Make
- Linux host running Docker

### Compile
```bash
make
```

### Install
```bash
sudo make install
```

### Usage
```bash
docker-event-listener [options]
```

Options:
- `-s <path>`: Docker UNIX socket path (default: `/var/run/docker.sock`)
- `-c <file>`: Rules configuration file (default: `/docker/config/events.conf`)
- `-d <dir>`: Dynamic hooks directory (default: `/docker/scripts/events.d`)
- `-v`: Enable verbose / debug logging
- `-h`: Show usage information

---

## Systemd Integration

```ini
[Unit]
Description=Extensible Docker Event Listener Daemon (Native C)
After=docker.service
Requires=docker.service

[Service]
Type=simple
User=aaxyat
Group=docker
Restart=always
RestartSec=3
ExecStart=/docker/scripts/docker-event-listener -c /docker/config/events.conf -d /docker/scripts/events.d
ExecReload=/bin/kill -HUP $MAINPID
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

---

## License
MIT © [Ayush Bhattarai](https://github.com/aaxyat)
