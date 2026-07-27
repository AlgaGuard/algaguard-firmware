# Development software identity local acceptance

Status: **PASS**

- Branch: `feat/secure-credential-storage`
- Git HEAD: `480131d1f94e68740c7dd41432cd9d8cc5d773a8`
- Worktree dirty: `true` (preserved phase work)
- Hardware runtime validation: not performed

## Test suites

- native: **144/144**
- Python guard tests: **7/7**
- artifact-security tests: **8/8**
- workflow static tests: **8/8**
- security preflight tests: **1/1**
- legacy artifact scanner tests: **3/3**
- partition board and image tests: **10/10**
- contract fixture guard: **PASS**

## Fresh target builds

| Environment | RAM | Flash | Image bytes | SHA-256 prefix | Profile |
|---|---:|---:|---:|---|---|
| `esp32-s3-devkitc-1` | 16908 | 226005 | 226432 | `303ef19f424a` | `DEFAULT_DEVELOPMENT` |
| `esp32-s3-dev-software-key` | 16908 | 226213 | 226640 | `e7cca741ecb3` | `INSECURE_DEVELOPMENT_IDENTITY` |
| `esp32-s3-dev-software-identity-self-test` | 16908 | 226213 | 226640 | `4948e5aa108b` | `INSECURE_DEVELOPMENT_SELF_TEST` |

## Acceptance gates

- Guard scenarios: **PASS** (7/7)
- Partitions: **PASS**
- TLS adapter: **TARGET_TLS_ADAPTER_COMPILE_PROVEN**
- Artifact staging: **PASS**
- Local CI path: **PASS**
- Secret scan: **PASS**
- Cleanup: **PASS**
- Diff/tooling: **PASS**
- Unresolved blockers: none
