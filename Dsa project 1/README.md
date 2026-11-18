cd "/mnt/d/SEM3/DSA/Dsa project 1/Dsa project 1"
cat > README.md << 'EOF'
# Time-Travel Debugger (TTDB)

A client-server debugger for the custom C-- language. The client uploads a program, the server runs it and returns a `.tdbg` file, and the client then moves through the execution history (next, prv, jmp) from that local file.

## Files

| File | Role |
|---|---|
| `server.cpp` | Receives the source, runs validation, resolve, execution and serialization, replies with the `.tdbg` file |
| `load_balancer.cpp` | Queues clients, starts the server if it is down, relays bytes in both directions |
| `client.cpp` | Interactive client: `start`, `next`, `prv`, `jmp` |
| `helper.h` | Shared constants, structs, CRC-32 and `TrailerChecker` |
| `test_crc.cpp` | Test for the checksum code |

## Build

    g++ -std=c++17 -Wall server.cpp -o server
    g++ -std=c++17 -pthread -Wall load_balancer.cpp -o load_balancer
    g++ -std=c++17 -Wall client.cpp -o client

## Run

    ./load_balancer
    ./client 127.0.0.1 5000

The load balancer listens on port 5000 and starts `./server 5001 &` when the server is down. The server executable must be in the folder the load balancer is started from.

## Client commands

| Command | Meaning |
|---|---|
| `start <file>` | Upload the source, download the `.tdbg`, show step 1 |
| `next` / `prv` | Move one step forward / back |
| `jmp next <n>` / `jmp prv <n>` | Move n steps forward / back |
| `help` | Show the commands |
| `quit` / `exit` | Leave |

Only `start` uses the network. The other commands read `ttdb_last.tdbg` from disk through its index.

## Wire format

Every message on every hop is `[status: 1 byte][payload][CRC-32: 4 bytes]`. Status 0 means the payload is the `.tdbg` file, status 1 means it is an error text. The CRC covers the status byte and the payload.

## Checksum

CRC-32 (the standard polynomial used by zip and png), little-endian trailer. The sender updates the CRC chunk by chunk and appends it last. The receiver uses `TrailerChecker`, which always holds back the last 4 bytes and compares them with the CRC of everything before them. The load balancer verifies each stream while forwarding and withholds the trailer if the check fails, so the next receiver rejects the message.

## Error cases

| Case | Behaviour |
|---|---|
| Queue full | `server busy, try again later` |
| Server cannot start or connect | `backend server failed to start` |
| Client waited too long in queue | Socket closed, no reply (the client reports a timeout) |
| Upload checksum mismatch | `upload checksum mismatch` sent to the client |
| Reply checksum mismatch | Client deletes the file, prints `checksum mismatch, response corrupted` |
| Bad `.tdbg` file | `server sent a damaged .tdbg file`, old session kept |

All six cases were tested.
EOF
printf "ttdb_last.tdbg.part\n" >> ../.gitignore
git add ../.gitignore client.cpp helper.h load_balancer.cpp server.cpp README.md
git status
git commit -m "Phase 2: LB relay, server networking, client, README"