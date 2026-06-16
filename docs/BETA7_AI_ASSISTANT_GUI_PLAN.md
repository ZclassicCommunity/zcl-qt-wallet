# Beta7 AI Assistant GUI Plan

Status: GUI implementation plan. AI disabled by default.
Scope: `/home/rhett/github/zcl-qt-wallet` with daemon facts supplied by
authenticated localhost RPC. The daemon does not call AI providers.

## Goals

Add an optional assistant that can help users understand and draft actions for:

- ZCL balances and sync status;
- transaction summaries;
- owned NFTs and collection state;
- ZMARKET listings, buy/sell drafts, and offer verification;
- ZNAM names and badges;
- mirror/content health;
- future social/gallery records.

The assistant can use user-provided credentials for OpenAI/GPT, Claude, Gemini,
z.ai, or a local/OpenAI-compatible endpoint. The provider layer is replaceable.

## Hard Rules

- AI cannot sign, broadcast, publish, host, unhost, post, or change settings.
  Beta7 exposes read-only tools and local draft records only; execution remains
  outside the model tool registry.
- AI never receives wallet seed, spending keys, viewing keys, transparent WIFs,
  RPC password, Tor private keys, onion private keys, or raw config.
- Provider credentials stay in GUI secure storage, never daemon config. If OS
  keychain support is not ready for beta7 packaging, use session-only
  credentials rather than persistent plaintext.
- Remote NFT metadata, market text, mirror records, ZNAM text, and social posts
  are untrusted data. They cannot override tool permissions.
- Existing wallet dialogs remain final authority for money-moving actions.

## Qt Classes

Suggested classes:

- `AiAssistantPanel`
- `AiChatModel`
- `AiProvider`
- `AiProviderRegistry`
- `OpenAIProvider`
- `ClaudeProvider`
- `GeminiProvider`
- `ZaiProvider`
- `LocalAiProvider`
- `AiCredentialStore`
- `AiContextBuilder`
- `AiToolRegistry`
- `AiCommandDraft`
- `AiApprovalDialog`
- `AiAuditLogModel`

Use ActiveRecord-style models for local rows only:

- `AiCredentialRecord`
- `AiConversationRecord`
- `AiToolCallRecord`
- `AiCommandRecord`
- `AiAuditRecord`

Do not add a GUI-side market spider/indexer or daemon replacement.

## Provider Boundary

The provider interface should hide vendor details:

```text
AiProvider::validateCredentials()
AiProvider::complete(AiRequest)
AiProvider::stream(AiRequest, AiStreamSink)
```

Provider adapters own endpoints, headers, model names, streaming formats, and
errors. The rest of the wallet sees normalized messages, tool-call requests, and
text output.

## Tool Boundary

Tools are REST-shaped wallet actions, not raw RPC passthrough:

```text
wallet/status
wallet/balances
nft/owned
nft/get
market/search
market/get
market/listing-drafts
market/buy-drafts
content/host-drafts
znam/resolve
social/post-drafts
```

Read tools can return bounded redacted summaries. Draft tools create local
command objects. Side-effect execution is owned by existing GUI confirmation
flows, not by provider tool calls.

## UX

Settings:

- AI enabled/disabled;
- provider selection;
- credential storage mode;
- remote provider allowed;
- local provider URL;
- wallet context access level;
- max context size;
- per-tool permission toggles;
- audit log retention.

Assistant panel:

- chat history;
- context chips showing what data categories are included;
- source badges for wallet, NFT, ZMARKET, ZNAM, mirror, social;
- command draft cards;
- approval buttons with full previews;
- audit trail link.

Never hide side effects inside chat text. A buy/list/host/post/change-settings
proposal must become a visible command card, and beta7 may keep these cards
disabled until approval-flow tests are complete.

## Tests

Unit:

- provider registry and disabled mode;
- credential encryption/redaction;
- context builder never includes secrets;
- tool permission matrix;
- command state machine;
- prompt-injection fixtures from NFT/social/market text;
- audit log redaction.

Widget:

- provider setup with fake provider;
- read-only assistant answers from mocked RPC data;
- AI-proposed buy creates a draft card only;
- reject approval leaves state unchanged;
- approve still opens existing wallet confirmation;
- locked wallet blocks spend execution;
- metadata instruction injection does not gain tool permissions.

Manual:

- fresh profile has AI disabled;
- no provider credential means no remote call;
- provider call can be routed through configured proxy/Tor policy if enabled;
- support bundle/log export contains no credentials or secrets.
