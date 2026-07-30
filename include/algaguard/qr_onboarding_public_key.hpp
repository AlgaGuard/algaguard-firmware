#pragma once

namespace algaguard {

// Public verification key only. The corresponding development signing key is
// held by the backend secret store and is never embedded in firmware.
inline constexpr char kQrOnboardingSigningPublicKeyPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE0G4voCTDmnITsWXyqHTEb/ZJhAVB\n"
    "uViRdQKo7ZTcCXOO7JQWfWOyLBmCN9MJHxEIVPjPmJqZNd68mRQ1mFSI9A==\n"
    "-----END PUBLIC KEY-----\n";

}  // namespace algaguard
