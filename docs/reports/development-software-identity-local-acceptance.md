# Development software identity local acceptance

Status: **PASS**

- Branch: `feat/secure-credential-storage`
- Git HEAD: `b2883ad62353a8745dee5fec96b0689254200519`
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
| `esp32-s3-devkitc-1` | 16908 | 226005 | 226432 | `1f3dcf0c784f` | `DEFAULT_DEVELOPMENT` |
| `esp32-s3-dev-software-key` | 16908 | 226213 | 226640 | `bdb6b22bf309` | `INSECURE_DEVELOPMENT_IDENTITY` |
| `esp32-s3-dev-software-identity-self-test` | 16908 | 226213 | 226640 | `e47ec0848b14` | `INSECURE_DEVELOPMENT_SELF_TEST` |

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
