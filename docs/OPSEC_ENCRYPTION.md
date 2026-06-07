# OPSEC: Encryption of wallet data at rest

> **Status: implemented** (`src/securestore.{h,cpp}` + store refactors). Branch `opsec-encryption`.

## 1. Threat model

A local adversary who can **read the user's disk or a backup of it** (stolen/seized laptop, leaked
cloud backup, forensic image, a shared/again-sold machine) must learn **nothing** about the user's
shielded activity from the files this GUI writes. The daemon's `wallet.dat` (spend keys) is out of
scope here — that's protected by the node's own `encryptwallet` (the future "multi-cipher wallet
encryption"). This document covers everything the **GUI** writes.

## 2. Audit — what the GUI wrote, and the fix

All live in the app-data dir (Linux `~/.local/share/zcl-qt-wallet-org/…`).

| File | Was | Sensitivity | Now |
|---|---|---|---|
| `senttxstore.dat` | JSON plaintext: shielded send history (from/to z-addrs, amounts, fees, txids, times) | 🔴 critical — the daemon can't even see z→z sends; this was the only record | **`senttxstore.enc`**, encrypted |
| `addresslabels.dat` | QDataStream plaintext: address ↔ human label | 🔴 critical — deanonymizes addresses | **`addresslabels.enc`**, encrypted |
| `turnstilemigrationplan.dat` | QDataStream plaintext: z→t→z migration plan | 🟠 high — reveals linkage | **`turnstilemigrationplan.enc`**, encrypted |
| `zcl-qt-wallet.log` | plaintext diagnostics | 🟡 medium — persistent | **OFF by default** (opt-in `options/savedebuglog`) |
| QSettings `connection/rpcpassword` | plaintext INI | 🟡 medium — daemon RPC creds | **residual, documented below** |

**Residual (honest):** the daemon's RPC password also lives in `zclassic.conf` in plaintext because
**the daemon itself must read it** — we cannot encrypt that without breaking the node. Encrypting only
the GUI's QSettings copy would be theatre while the same secret sits in `zclassic.conf`. Mitigation:
the RPC password gates *node access*, not spend keys, and our app-data dir/files are written `0700`/
`0600` (owner-only). A future option is to stop the GUI duplicating it into QSettings and read it from
`zclassic.conf` on demand. User-initiated **CSV export** and **wallet.dat backup** write to a path the
user chooses — deliberate exports, not silent leaks.

## 3. SecureStore — the encryption core

One master secret, derived once at startup, protects every file via a single container format.

### KDF (the real front line)
- **Argon2id** (`crypto_pwhash`), **SENSITIVE** params (~1 GiB RAM, multi-second) → 256-bit master
  key. Memory-hard ⇒ brute-force is dominated by RAM cost, not cipher speed.
- **Strict: SENSITIVE only — no weaker tier.** There is intentionally no MODERATE option and no
  silent downgrade: we never weaken the brute-force cost behind the user's back. If a machine truly
  cannot allocate the ~1 GiB, setup **fails loudly** rather than producing a weaker key.
- The exact parameters are recorded in the verifier, so unlock always reproduces the key.
- Salt is random per wallet, stored in the verifier file.
- Because derivation takes a few seconds, both setup and unlock run it on a **worker thread behind a
  splash** (ZCL logo + "Decrypting…/Encrypting…" + animated progress) so the UI never freezes blankly.

### Cipher — a two-layer AEAD cascade (defense-in-depth)
Independent subkeys are derived from the master key with `crypto_kdf` (BLAKE2b), context `zclstore`:

```
plaintext
  → inner: XChaCha20-Poly1305  (stream family, 192-bit random nonce, misuse-resistant)
  → outer: AES-256-GCM         (block family — cipher-family diversity)
  = blob
```

A cryptanalytic break of **either** cipher still leaves the data sealed by the other. Where AES-NI is
unavailable (e.g. some ARM), the outer layer falls back to a **second, independent-key**
XChaCha20-Poly1305; the choice is recorded in the blob's `mode` byte so files stay self-describing.

**Quantum:** for password-based *symmetric* encryption, Grover only halves brute-force strength, so
256-bit keys give ~128-bit post-quantum security — ample. (Shor threatens *public-key* crypto, which
we don't use here.) The cascade is about *classical* future-proofing, not quantum.

### Blob format
```
'Z''C''S' | ver(1) | mode(1) | inner_nonce(24) | outer_nonce(12|24) | ciphertext
  mode 0 = XChaCha20-Poly1305 then AES-256-GCM
  mode 1 = XChaCha20-Poly1305 then XChaCha20-Poly1305 (independent key)
```

### Verifier (`master.bin`)
```
"ZCK1" | ver(1) | secretType(1) | salt(16) | argon2_ops(8) | argon2_mem(8) | enc(canary)
```
Unlock = derive key from the supplied secret + stored salt/params, then AEAD-decrypt the canary; the
Poly1305/GCM tag *is* the password check (no plaintext comparison of secrets).

### Key hygiene
Master key is held in `sodium_mlock`'d memory for the session and `sodium_memzero`'d on lock/exit.
Subkeys and secret buffers are zeroed immediately after use. Files are written atomically
(`QSaveFile`) with `0600` perms; the data dir is `0700`.

## 4. Unlock factors (password / keyfile / both)

At first run the user picks:
- **Password** — something memorised.
- **Keyfile** — any file the user keeps elsewhere (USB stick…); its full contents are BLAKE2b-hashed
  and stretched by Argon2id like a password. Nothing to memorise, but **lose it = lose access**.
- **Both** — bound together as `Argon2id( BLAKE2b(password ‖ keyfileHash) )`; a stolen disk needs the
  keyfile *and* the password.

The chosen type is recorded in the verifier so later unlocks prompt for the right factor(s).

## 5. First-run, unlock, and reset behavior

- **First run** shows a notice (history, labels and the turnstile plan will be encrypted; the same
  secret will later unlock wallet key encryption; **there is no recovery**), then the factor chooser
  + setup.
- **Returning runs** prompt to unlock *before any window or store touches disk*. Cancelling/Quitting
  **refuses to start** — the app never falls back to writing plaintext.
- **Wrong secret** → Try again / **Reset** / Quit. Reset (double-confirmed) purges all managed
  encrypted files + the verifier and drops into fresh setup. **Coins are unaffected** — they live in
  `wallet.dat`, which this never touches.
- **Legacy migration:** on first unlock, any existing plaintext `*.dat` is read, re-written as `*.enc`,
  and the cleartext original is deleted — transparent in-place upgrade.

## 6. Known follow-ups
- A Settings checkbox to toggle the (default-off) plaintext debug log.
- Optionally stop duplicating the RPC password into QSettings (read from `zclassic.conf`).
- Re-lock / re-prompt on a configurable idle timeout.
