# Bitcoin Echo

A complete, ossified implementation of the Bitcoin protocol in pure C.

*Build once. Build right. Stop.*

## Status

**Phase 2: Cryptographic Primitives** — Complete (110 tests passing)

| Component | Tests |
|-----------|-------|
| SHA-256 | 9/9 |
| RIPEMD-160 | 17/17 |
| secp256k1 Field | 19/19 |
| secp256k1 Group | 15/15 |
| ECDSA Verification | 17/17 |
| Schnorr (BIP-340) | 20/20 |
| Signature Interface | 13/13 |

Next: Phase 3 — Consensus Data Structures

## Building

### POSIX (Linux, macOS, BSD)

```sh
make
./echo
```

### Windows

```cmd
build.bat
echo.exe
```

## Testing

```sh
make test
```

Runs all unit tests for cryptographic primitives.

## Requirements

- C11 compiler (GCC, Clang, or MSVC)
- No external dependencies

## Documentation

- [Whitepaper](https://bitcoinecho.org) — Technical specification
- [Manifesto](https://bitcoinecho.org) — Philosophical foundation

## License

MIT License — see [LICENSE](LICENSE)
