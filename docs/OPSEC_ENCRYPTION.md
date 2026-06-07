# OPSEC: Encryption of wallet data at rest

> **Status: implemented, OPT-IN (default OFF)** — `src/securestore.{h,cpp}` + store refactors.
> Originated as community PR #27 (encrypt-at-rest, *hairetikos*); reworked here to be opt-in,
> sub-second, headless-safe, and data-loss-safe before integration.

## 1. Threat model

A local adversary who can **read the user's disk or a backup of it** (stolen/seized laptop,
leaked cloud backup, forensic image, a shared/resold machine) must learn **nothing** about the
user's shielded activity from the files the **GUI** writes. Out of scope: the daemon's
`wallet.dat` (spend keys — protected by the node's own `encryptwallet`) and the RPC password in
`zclassic.conf` (the node itself must read it).

## 2. Audit — what the GUI wrote, and the fix

All live in the app-data dir (Linux `~/.local/share/zcl-qt-wallet-org/…`).

| File | Was | Sensitivity | Now (when encryption is ON) |
|---|---|---|---|
| `senttxstore.dat` | JSON plaintext: shielded send history (to z-addrs, amounts, fees, txids, times) | 🔴 critical — the daemon can't even see z→z sends; this is the *only* record | `senttxstore.enc`, encrypted |
| `addresslabels.dat` | QDataStream plaintext: address ↔ human label | 🔴 critical — deanonymizes addresses | `addresslabels.enc`, encrypted |
| `turnstilemigrationplan.dat` | QDataStream plaintext: z→t→z migration plan | 🟠 high — reveals linkage | `turnstilemigrationplan.enc`, encrypted |
| `zcl-qt-wallet.log` | plaintext diagnostics | 🟡 medium — persistent | **OFF by default** (opt-in `options/savedebuglog`) |

When encryption is **OFF (the default)** those three files are still written **owner-only
(0600) plaintext**, atomically — exactly as before this change.

**Residual (honest):** the daemon's RPC password also lives in `zclassic.conf` in plaintext
because **the daemon itself must read it**. Encrypting only the GUI's copy would be theatre.
User-initiated **CSV export** and **wallet.dat backup** write to a user-chosen path — deliberate
exports, not silent leaks.

## 3. Opt-in, not mandatory (the key design choice)

Encryption is **off by default** and enabled from **Settings → "Encrypt my wallet data with a
password."** This is deliberate:

- The wallet's north star is *instant startup / don't-make-me-think*. A mandatory password +
  key-derivation on every launch would fight that for **every** user, including those who don't
  share their machine.
- A pinned heavy key-derivation can **lock a user out of their own irreplaceable history** on a
  lower-RAM machine. We never want a security feature to cause data **un**availability for
  someone who simply enabled it.

So: those who want it turn it on; everyone else pays nothing and risks nothing. The setting
takes effect on the **next launch** (first launch sets the password; later launches unlock).

## 4. SecureStore — the encryption core

One master secret, derived once at unlock, protects every managed file via a single container.

### KDF (the real front line)
- **Argon2id** (`crypto_pwhash`), **MODERATE** params (~256 MiB, **sub-second**) →
  256-bit master key. Memory-hard ⇒ brute-force cost is dominated by RAM, not cipher speed.
- Sub-second so it never freezes startup; 256 MiB so it never locks a usable machine out of its
  own history. The **exact ops/mem are recorded in the verifier**, so unlock always reproduces
  the key and a future "maximum-strength" tier can coexist file-by-file without breaking
  existing data.
- Salt is random per wallet, stored in the verifier.
- Derivation runs on a **worker thread behind an animated splash** so the UI never freezes.

### Cipher — a single modern AEAD
A subkey is derived from the master key with `crypto_kdf` (BLAKE2b, context `zclstore`), then:

```
plaintext → XChaCha20-Poly1305 (192-bit random nonce, misuse-resistant) → blob
```

XChaCha20-Poly1305 is a strong modern AEAD; one layer is ample. The earlier proposal added an
AES-256-GCM *outer* layer for "cipher-family diversity", but that made a file written on an
AES-NI machine **unreadable on a machine without AES-NI** (a real cross-machine lock-out) for
~0 real-world security gain, so it was dropped. **256-bit keys ⇒ ~128-bit post-quantum
(Grover)** — ample for symmetric, password-based encryption. (Shor threatens *public-key*
crypto, which this does not use.)

### Blob format
```
'Z''C''S' | ver(1) | nonce(24) | ciphertext(+16B Poly1305 tag)
```
The 4-byte header (`magic | ver`) is passed as **AEAD associated data**, so the format
descriptor is authenticated, not just the payload.

### Verifier (`master.bin`)
```
"ZCK1" | ver(1) | secretType(1) | salt(16) | argon2_ops(8) | argon2_mem(8) | enc(canary)
```
Unlock = derive the key from the supplied secret + stored salt/params, then AEAD-decrypt the
canary; the Poly1305 tag **is** the password check (no plaintext comparison of secrets).

### Key hygiene
Master key held in `sodium_mlock`'d memory and `sodium_memzero`'d on lock/exit. Subkeys and the
keyfile-hash buffer are zeroed immediately after use; the cleartext password is cleared after
derivation (Qt's `QString` is best-effort here). Files are written atomically (`QSaveFile`)
`0600`; the data dir is `0700`.

## 5. Unlock factors (password / keyfile / both)

- **Password** — something memorised.
- **Keyfile** — any file the user keeps elsewhere (USB stick…); its full contents are
  BLAKE2b-hashed and stretched by Argon2id like a password. **Lose it = lose access.**
- **Both** — bound as `Argon2id( BLAKE2b(password ‖ keyfileHash) )`; a stolen disk needs the
  keyfile *and* the password.

The chosen type is recorded in the verifier so later unlocks prompt for the right factor(s).

## 6. First-run, unlock, and reset behavior

- **First run** (after the user opts in): a short notice (history + labels will be encrypted;
  **there is no recovery**; coins are safe in `wallet.dat`), then the factor chooser + set-up.
- **Returning runs** prompt to unlock *before any window or store touches disk*. Cancelling
  refuses to start — never falls back to plaintext.
- **Wrong secret** → Try again / **Reset** / Quit. Reset (double-confirmed) purges all managed
  encrypted files + the verifier and drops into fresh setup. **Coins are unaffected.**
- **Legacy migration:** on first unlock, an existing plaintext `*.dat` is read, re-written as
  `*.enc`, and the cleartext original is best-effort wiped (see §7).
- **Corruption safety:** if an `.enc` exists but won't decrypt (genuine corruption — the key is
  correct once unlocked), the stores **refuse to overwrite it** and surface nothing as empty, so
  the only record of past shielded sends is never silently destroyed. The escape hatch is the
  Settings reset.

## 7. Honest residuals & limitations

- **Deleted plaintext is not guaranteed gone.** Legacy `.dat` migration and reset overwrite the
  file with zeros before unlinking (`secureRemove`), which raises the bar on simple filesystems,
  but on **copy-on-write / log-structured / SSD-wear-levelled** media the old bytes may remain
  recoverable from a forensic image until the space is reused. **Full-disk encryption is the
  only complete defense** against the forensic-image adversary; this feature reduces, not
  eliminates, that exposure for pre-existing plaintext.
- The RPC password residual in `zclassic.conf` (see §2).
- Memory: the cleartext password transits a Qt `QString`, which does not guarantee zeroing on
  destruction (defense-in-depth gap, not a confidentiality break; the master key itself is
  mlock'd and zeroed).

## 8. Build / test / headless

- Links the **already-bundled libsodium** (`res/libsodium.a`, 1.0.16) — no new dependency.
- **Headless / automation never prompts:** the startup gate is skipped under `--headless` and
  when `ZQW_NO_SECURESTORE` is set (the offscreen E2E seam), and of course when the user hasn't
  opted in.
- L0 unit suite links no libsodium; a transparent passthrough shim (`tests/shim/securestore.h`)
  preserves the on-disk contract (0600, testnet-prefixed, atomic) for the default plaintext path.
  The AEAD itself is validated against real libsodium (round-trip across modes, wrong-key
  rejection, single-bit tamper detection, empty input).
