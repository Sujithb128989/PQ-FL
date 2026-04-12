#ifndef CRYPTO_ENGINE_HPP
#define CRYPTO_ENGINE_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

// AES-256-GCM encryption for weight payloads and checkpoints.
// Session keys are derived from a shared secret + per-RPC context.

class CryptoEngine {
public:
    CryptoEngine(const std::string& key_path, const std::string& payload_key_path = "");
    ~CryptoEngine();

    // Encrypts plaintext weights. Returns ciphertext only.
    // Outputs IV (12 bytes) and Tag (16 bytes) separately for proto packing.
    std::string Encrypt(const std::string& plaintext,
                        std::vector<unsigned char>& iv_out,
                        std::vector<unsigned char>& tag_out);

    // Decrypts ciphertext given IV and Tag.
    std::string Decrypt(const std::string& ciphertext,
                        const std::vector<unsigned char>& iv,
                        const std::vector<unsigned char>& tag);

    // Legacy convenience: Encrypt returning IV+Ciphertext+Tag blob.
    std::string EncryptBlob(const std::string& plaintext);

    // Legacy convenience: Decrypt from IV+Ciphertext+Tag blob.
    std::string DecryptBlob(const std::string& blob);

    int GetActiveKeyVersion() const;
    int RotateMasterKey();

    std::vector<unsigned char> DeriveSessionKey(const std::vector<std::string>& context_parts) const;

    // Load a shared application payload secret from disk for demo/test clients.
    static std::vector<unsigned char> LoadSecretFile(const std::string& path);
    static std::vector<unsigned char> DeriveSessionKeyWithSecret(
        const std::vector<unsigned char>& secret,
        const std::vector<std::string>& context_parts);

    // Encrypt with a specific key (for per-client session keys).
    static std::string EncryptWithKey(const unsigned char* key,
                                      const std::string& plaintext,
                                      std::vector<unsigned char>& iv_out,
                                      std::vector<unsigned char>& tag_out);

    // Decrypt with a specific key.
    static std::string DecryptWithKey(const unsigned char* key,
                                      const std::string& ciphertext,
                                      const std::vector<unsigned char>& iv,
                                      const std::vector<unsigned char>& tag);

private:
    std::string key_path_;
    std::string key_registry_path_;
    std::string payload_key_path_;
    mutable std::mutex mutex_;
    unsigned char key_[32]; // AES-256 master key
    int active_key_version_ = 1;
    std::unordered_map<int, std::vector<unsigned char>> key_versions_;
    std::vector<unsigned char> payload_secret_;

    void LoadOrGenerateKey();
    void LoadOrGeneratePayloadSecret();
    void SaveKeyRegistry() const;
    std::string VersionedKeyPath(int version) const;
};

#endif // CRYPTO_ENGINE_HPP
