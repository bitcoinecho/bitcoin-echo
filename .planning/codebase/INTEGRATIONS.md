# External Integrations

**Analysis Date:** 2026-02-20

## APIs & External Services

**Bitcoin P2P Network:**
- Service: Bitcoin mainnet, testnet3, regtest
- What it's used for: Peer-to-peer synchronization, block/transaction relay, network discovery
  - SDK/Client: Custom implementation in `src/protocol/` (no external library)
  - Protocol: Bitcoin P2P protocol (BIP-31, BIP-35, BIP-37, BIP-130, BIP-133, BIP-144, BIP-152)
  - Network magic bytes: Compile-time selected (`ECHO_NETWORK_MAGIC`)
  - Default ports: 8333/mainnet, 18333/testnet, 18444/regtest

**RPC API (Local):**
- Service: JSON-RPC 2.0 interface
- What it's used for: External programs query blockchain state, submit transactions, node control
  - Protocol: HTTP/1.0 on localhost (configurable port)
  - Ports: 8332/mainnet, 18332/testnet, 18443/regtest
  - Implementation: `src/app/rpc.c` (embedded JSON parser, no external library)
  - Methods: getsyncstatus, getblockchaininfo, getblockheader, getblock, getrawtransaction, submitblock, sendrawtransaction, getobserverstats (observer mode)
  - Max concurrent connections: 16 (PLATFORM_RPC_MAX_CONNECTIONS)

**DNS Seeds (Bootstrap):**
- Service: Bitcoin DNS seed nodes
- What it's used for: Peer discovery on network startup
  - Implementation: `src/protocol/discovery.c` queries hardcoded DNS seed hostnames
  - Mainnet seeds: seed.bitcoin.sipa.be, dnsseed.bluematt.me, dnsseed.coinmine.pl, dnsseed.emzy.de, dnsseed.petertodd.org, seed.btcnet.io, seed.bitcoin.sprouting.com, seed.bitnodes.io
  - Testnet seeds: testnet-seed.bitcoin.jonasschnelli.ch, seed.tbtc.petertodd.org, seed.testnet.bitcoin.sprouting.com, testnet-seed.bluematt.me
  - Regtest: No DNS seeds (hardcoded addresses only)
  - DNS resolution: Platform `plat_dns_resolve()` via POSIX `getaddrinfo()`

**Hardcoded Seed Addresses (Fallback):**
- Service: Built-in fallback peer list
- What it's used for: Bootstrap when DNS seeds unavailable
  - Implementation: `src/protocol/discovery.c` includes hardcoded IP:port pairs per network
  - Mainnet: ~25 hardcoded node addresses
  - Testnet: ~25 hardcoded node addresses
  - Regtest: 127.0.0.1:18444 (local)

## Data Storage

**Databases:**
- SQLite 3.x (embedded)
  - Connection: `~/.bitcoin-echo/chainstate/echo.db` (configurable via `--datadir`)
  - Client: libsqlite3 (vendored amalgamation in `lib/sqlite/sqlite3.c`)
  - Purpose: UTXO set (unspent outputs), block index, chainstate metadata
  - Interface: `src/storage/db.c` (transaction wrapper), `src/storage/utxo_db.c`, `src/storage/block_index_db.c`
  - Mode: WAL (Write-Ahead Logging) for better concurrency
  - Config: `synchronous=NORMAL` (good safety/performance balance), `foreign_keys=ON`

**File Storage:**
- Block files: `~/.bitcoin-echo/blocks/` (local filesystem only)
  - Format: blk000000.dat, blk000001.dat, etc. (Bitcoin Core compatible)
  - Purpose: Raw block data storage (~400 GB for mainnet archive)
  - Interface: `src/storage/blocks.c` (block file management)
  - Pruning: Optional via `--prune=<MB>` flag (minimum 550 MB for reorg safety)

**No Cloud or Remote Storage:**
- All data stored locally
- No cloud integrations (S3, Azure, etc.)
- No external database services

**Caching:**
- In-memory UTXO cache: Loaded from SQLite on startup
- In-memory mempool: `src/protocol/mempool.c` (max unconfirmed transactions)
- No external cache service (Redis, Memcached, etc.)

## Authentication & Identity

**Auth Provider:**
- None - RPC interface is local-only (no authentication)
- P2P network: No authentication (Bitcoin protocol operates on open network)
- Peer identity: Verified via P2P version exchange and network rules, not cryptographic auth

**Network Identity:**
- User agent: `/BitcoinEcho:0.0.1/` (sent in version messages)
- Unique node ID: Generated at runtime from network/port configuration

## Monitoring & Observability

**Error Tracking:**
- None - No external error tracking service
- Local logging: `src/app/log.c` writes to stderr and local log files
- Supported formats: Human-readable log lines with timestamps and severity

**Logs:**
- Approach: Direct file writing to `~/.bitcoin-echo/echo.log` (or stdout if no datadir)
- Log levels: ERROR (0), WARN (1), INFO (2), DEBUG (3)
- Default level: INFO (`PLATFORM_LOG_LEVEL = 2`)
- Rotation: Max 10 MB per file, keep 5 rotated files (configurable)
- No external logging service

**Health Checks:**
- RPC method `getsyncstatus` returns: headers synced, blocks synced, peer count, download rate
- RPC method `getblockchaininfo` returns: chain tip, difficulty, mediantime, verification progress

## CI/CD & Deployment

**Hosting:**
- No hosted deployment - Binary is self-contained
- Supported platforms: Linux, macOS, BSD, Windows (via platform abstraction)
- Deployment method: Single static binary + optional data directory

**CI Pipeline:**
- GitHub Actions (`.github/workflows/test.yml`)
  - Runs on: ubuntu-latest, macos-latest
  - Tests: `make test` with AddressSanitizer and LeakSanitizer
  - Build: `make all` for release binary
  - Triggers: on push to main, on pull requests
  - No external CI services beyond GitHub Actions

**Build Artifacts:**
- Single static binary: `echo`
- No dynamic library dependencies
- Requires system libc and libpthread

## Environment Configuration

**Required env vars:**
- `HOME` - Used to determine default data directory (`~/.bitcoin-echo`) if not specified
- `LSAN_OPTIONS` - For leak sanitizer suppressions in CI (`.supp` file)
- No other environment variables required

**Secrets location:**
- No secrets managed - node uses public keys and operates on open network
- RPC credentials: Not supported (local-only access)
- Private keys: Not stored (Bitcoin Echo is validation-only, no wallet)

**Network Access:**
- Outbound: TCP 8333 (mainnet P2P), plus DNS port 53 (resolution)
- Inbound: TCP 8333 (P2P listen), TCP 8332 (RPC listen) - local only
- No HTTP/HTTPS except RPC (basic HTTP/1.0)

## Webhooks & Callbacks

**Incoming:**
- RPC JSON-RPC 2.0 interface (`src/app/rpc.c`)
  - Endpoint: `http://localhost:8332/` (mainnet)
  - Methods: Standard Bitcoin RPC + Echo-specific observer methods
  - No incoming webhooks

**Outgoing:**
- None - Bitcoin Echo is pull-based, not push-based
- Does not initiate outbound requests except P2P peer connections
- No notification callbacks

## Data Flow: Peer-to-Network Integration

**Sync Flow:**
1. Node starts, queries DNS seeds (`src/protocol/discovery.c`)
2. Gets peer list, connects to 64 outbound peers (`src/protocol/peer.c`)
3. Receives `inv` messages with block hashes
4. Downloads block headers first (headers-first sync, BIP-144 compatible)
5. Validates headers (PoW, timestamp)
6. Downloads block bodies in parallel from multiple peers
7. Validates blocks (signatures, UTXO, merkle root) via consensus engine
8. Stores blocks to disk and updates chainstate in SQLite
9. Maintains mempool of unconfirmed transactions

**Block Relay:**
- Peers announce new blocks via `inv` messages
- Node requests block data via `getdata`
- Validates via `src/consensus/block_validate.c`
- Announces to other peers via `inv`

**Transaction Relay:**
- Peers announce transactions via `inv`
- Node validates via `src/consensus/tx_validate.c`
- Stores in mempool (`src/protocol/mempool.c`)
- Announces to other peers

**RPC Integration:**
- External tools query node status via HTTP RPC
- Requests routed to `src/app/rpc.c`
- Responses constructed from node state (chainstate, peers, mempool)

## Network Topology

**P2P Peer Connections:**
- Max outbound: 64 peers (aggressive IBD parallelism)
- Max inbound: 100 peers
- Connection timeout: 5000 ms
- Inactivity timeout: 600000 ms (10 minutes)
- Ping interval: 120000 ms (2 minutes keepalive)
- Peer address manager: Stores up to 65536 known peers

**Network Modes:**
- **Full node (default)**: Downloads and validates all blocks, stores locally
- **Observer mode** (`--observe`): Connects to network, observes traffic, no validation/storage
- **Pruned node** (`--prune=<MB>`): Full validation but keeps only recent blocks

---

*Integration audit: 2026-02-20*
