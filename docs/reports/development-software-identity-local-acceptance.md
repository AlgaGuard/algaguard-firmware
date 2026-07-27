# Development software identity local acceptance

Status: **PASS**

- Branch: `feat/secure-credential-storage`
- Git HEAD: `a46eccb0cffc5cfa90e34199204c0e1ad11eb26e`
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
| `esp32-s3-devkitc-1` | 16908 | 226005 | 226432 | `b831d527562e` | `DEFAULT_DEVELOPMENT` |
| `esp32-s3-dev-software-key` | 16908 | 226213 | 226640 | `ab5f7b2e7021` | `INSECURE_DEVELOPMENT_IDENTITY` |
| `esp32-s3-dev-software-identity-self-test` | 16908 | 226213 | 226640 | `eaa42b4f87c3` | `INSECURE_DEVELOPMENT_SELF_TEST` |

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
