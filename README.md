# Distributed Seat Reservation System (Advanced System C)

This project implements a high-performance seat reservation engine using POSIX System C primitives:
- **I/O Multiplexing:** `poll()` event loop handling multiple non-blocking sockets.
- **Granular Record Locking:** Record-level byte offset locks using `fcntl(fd, F_SETLK, &fl)` to eliminate coarse-grained table locks.
- **Binary Disk Persistence:** Direct `pread()` / `pwrite()` and fixed-stride data structures.
- **Binary Wire Protocol:** Structured packets with fixed 12-byte headers (`magic`, `opcode`, `status`, `payload_len`).

## How to Build and Run

1. Build the executables:
   ```bash
   cc -o server server.c
   cc -o client client.c
   ```

2. Start the server:
   ```bash
   ./server
   ```

3. In one or more separate terminals, run the client:
   ```bash
   ./client
   ```

### Default Credentials
- **Passenger 1:** `passenger1` / `pass123`
- **Passenger 2:** `passenger2` / `pass123`
- **Admin:** `admin1` / `admin123`
