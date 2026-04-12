#include "crypto_engine.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

constexpr int KEY_SIZE = 32;
constexpr int IV_SIZE = 12;
constexpr int TAG_SIZE = 16;
constexpr unsigned char BLOB_MAGIC[] = {'P', 'Q', 'F', '2'};
constexpr size_t BLOB_HEADER_SIZE = 8;

namespace {

uint32_t ReadUint32BigEndian(const unsigned char* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void WriteUint32BigEndian(unsigned char* out, uint32_t value) {
    out[0] = static_cast<unsigned char>((value >> 24) & 0xFF);
    out[1] = static_cast<unsigned char>((value >> 16) & 0xFF);
    out[2] = static_cast<unsigned char>((value >> 8) & 0xFF);
    out[3] = static_cast<unsigned char>(value & 0xFF);
}

} // namespace

CryptoEngine::CryptoEngine(const std::string& key_path, const std::string& payload_key_path)
    : key_path_(key_path),
      key_registry_path_(key_path + ".meta.json"),
      payload_key_path_(payload_key_path.empty() ? key_path + ".payload" : payload_key_path) {
    LoadOrGenerateKey();
}

CryptoEngine::~CryptoEngine() {
    OPENSSL_cleanse(key_, sizeof(key_));
    for (auto& [_, key_bytes] : key_versions_) {
        if (!key_bytes.empty()) {
            OPENSSL_cleanse(key_bytes.data(), key_bytes.size());
        }
    }
    if (!payload_secret_.empty()) {
        OPENSSL_cleanse(payload_secret_.data(), payload_secret_.size());
    }
}

std::string CryptoEngine::VersionedKeyPath(int version) const {
    std::filesystem::path path(key_path_);
    std::string stem = path.stem().string();
    std::string ext = path.extension().string();
    return (path.parent_path() / (stem + ".v" + std::to_string(version) + ext)).string();
}

void CryptoEngine::SaveKeyRegistry() const {
    json j;
    j["active_key_version"] = active_key_version_;
    j["keys"] = json::array();
    for (const auto& [version, _] : key_versions_) {
        j["keys"].push_back({
            {"version", version},
            {"path", VersionedKeyPath(version)}
        });
    }

    std::filesystem::create_directories(std::filesystem::path(key_registry_path_).parent_path());
    std::ofstream registry_file(key_registry_path_, std::ios::trunc);
    registry_file << j.dump(2);
}

void CryptoEngine::LoadOrGenerateKey() {
    std::lock_guard<std::mutex> lock(mutex_);
    key_versions_.clear();

    auto load_key_bytes = [&](const std::string& path) -> std::vector<unsigned char> {
        std::ifstream key_file(path, std::ios::binary);
        if (!key_file.is_open()) {
            throw std::runtime_error("Failed to open key file: " + path);
        }

        std::vector<unsigned char> key_bytes(KEY_SIZE);
        key_file.read(reinterpret_cast<char*>(key_bytes.data()), KEY_SIZE);
        if (key_file.gcount() != KEY_SIZE) {
            throw std::runtime_error("Corrupted key file: " + path);
        }
        return key_bytes;
    };

    if (std::filesystem::exists(key_registry_path_)) {
        std::ifstream registry_file(key_registry_path_);
        json j;
        registry_file >> j;
        active_key_version_ = j.value("active_key_version", 1);

        for (const auto& entry : j["keys"]) {
            int version = entry.value("version", 0);
            std::string path = entry.value("path", "");
            if (version <= 0 || path.empty()) {
                continue;
            }
            key_versions_[version] = load_key_bytes(path);
        }
    } else if (std::filesystem::exists(key_path_)) {
        active_key_version_ = 1;
        key_versions_[1] = load_key_bytes(key_path_);
        std::filesystem::copy_file(
            key_path_,
            VersionedKeyPath(1),
            std::filesystem::copy_options::overwrite_existing
        );
        SaveKeyRegistry();
    } else {
        std::cout << "Generating new AES-256 master key..." << std::endl;
        std::vector<unsigned char> key_bytes(KEY_SIZE);
        if (RAND_bytes(key_bytes.data(), KEY_SIZE) != 1) {
            throw std::runtime_error("Failed to generate random key.");
        }

        std::filesystem::create_directories(std::filesystem::path(key_path_).parent_path());
        std::ofstream out_file(key_path_, std::ios::binary);
        out_file.write(reinterpret_cast<const char*>(key_bytes.data()), KEY_SIZE);
        std::filesystem::permissions(key_path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);

        active_key_version_ = 1;
        key_versions_[1] = key_bytes;
        std::ofstream versioned_file(VersionedKeyPath(1), std::ios::binary);
        versioned_file.write(reinterpret_cast<const char*>(key_bytes.data()), KEY_SIZE);
        SaveKeyRegistry();
    }

    auto active_it = key_versions_.find(active_key_version_);
    if (active_it == key_versions_.end()) {
        throw std::runtime_error("Active key version missing from key registry.");
    }

    std::memcpy(key_, active_it->second.data(), KEY_SIZE);
    LoadOrGeneratePayloadSecret();
}

void CryptoEngine::LoadOrGeneratePayloadSecret() {
    payload_secret_ = LoadSecretFile(payload_key_path_);
    if (!payload_secret_.empty()) {
        return;
    }

    payload_secret_.assign(KEY_SIZE, 0);
    if (RAND_bytes(payload_secret_.data(), KEY_SIZE) != 1) {
        throw std::runtime_error("Failed to generate payload encryption secret.");
    }

    std::filesystem::create_directories(std::filesystem::path(payload_key_path_).parent_path());
    std::ofstream out_file(payload_key_path_, std::ios::binary | std::ios::trunc);
    out_file.write(reinterpret_cast<const char*>(payload_secret_.data()), payload_secret_.size());
    out_file.close();

    std::filesystem::permissions(payload_key_path_,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
}

std::string CryptoEngine::Encrypt(const std::string& plaintext,
                                  std::vector<unsigned char>& iv_out,
                                  std::vector<unsigned char>& tag_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    return EncryptWithKey(key_, plaintext, iv_out, tag_out);
}

std::string CryptoEngine::Decrypt(const std::string& ciphertext,
                                  const std::vector<unsigned char>& iv,
                                  const std::vector<unsigned char>& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DecryptWithKey(key_, ciphertext, iv, tag);
}

std::string CryptoEngine::EncryptBlob(const std::string& plaintext) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<unsigned char> iv, tag;
    std::string ciphertext = EncryptWithKey(key_, plaintext, iv, tag);

    std::string blob;
    blob.reserve(BLOB_HEADER_SIZE + IV_SIZE + ciphertext.size() + TAG_SIZE);
    blob.append(reinterpret_cast<const char*>(BLOB_MAGIC), sizeof(BLOB_MAGIC));

    unsigned char version_bytes[4];
    WriteUint32BigEndian(version_bytes, static_cast<uint32_t>(active_key_version_));
    blob.append(reinterpret_cast<const char*>(version_bytes), sizeof(version_bytes));
    blob.append(reinterpret_cast<const char*>(iv.data()), iv.size());
    blob.append(ciphertext);
    blob.append(reinterpret_cast<const char*>(tag.data()), tag.size());
    return blob;
}

std::string CryptoEngine::DecryptBlob(const std::string& blob) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (blob.size() >= BLOB_HEADER_SIZE + IV_SIZE + TAG_SIZE &&
        std::memcmp(blob.data(), BLOB_MAGIC, sizeof(BLOB_MAGIC)) == 0) {
        uint32_t version = ReadUint32BigEndian(
            reinterpret_cast<const unsigned char*>(blob.data() + sizeof(BLOB_MAGIC))
        );
        auto it = key_versions_.find(static_cast<int>(version));
        if (it == key_versions_.end()) {
            throw std::runtime_error("Unknown key version for encrypted blob");
        }

        const size_t payload_offset = BLOB_HEADER_SIZE;
        std::vector<unsigned char> iv(blob.begin() + payload_offset,
                                      blob.begin() + payload_offset + IV_SIZE);
        std::vector<unsigned char> tag(blob.end() - TAG_SIZE, blob.end());
        std::string ciphertext(blob.begin() + payload_offset + IV_SIZE, blob.end() - TAG_SIZE);
        return DecryptWithKey(it->second.data(), ciphertext, iv, tag);
    }

    if (blob.size() < IV_SIZE + TAG_SIZE) {
        throw std::runtime_error("Invalid ciphertext blob length");
    }

    std::vector<unsigned char> iv(blob.begin(), blob.begin() + IV_SIZE);
    std::vector<unsigned char> tag(blob.end() - TAG_SIZE, blob.end());
    std::string ciphertext(blob.begin() + IV_SIZE, blob.end() - TAG_SIZE);
    return DecryptWithKey(key_, ciphertext, iv, tag);
}

int CryptoEngine::GetActiveKeyVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_key_version_;
}

int CryptoEngine::RotateMasterKey() {
    std::lock_guard<std::mutex> lock(mutex_);

    int new_version = active_key_version_ + 1;
    std::vector<unsigned char> key_bytes(KEY_SIZE);
    if (RAND_bytes(key_bytes.data(), KEY_SIZE) != 1) {
        throw std::runtime_error("Failed to generate rotated key.");
    }

    std::ofstream key_file(VersionedKeyPath(new_version), std::ios::binary);
    key_file.write(reinterpret_cast<const char*>(key_bytes.data()), KEY_SIZE);

    key_versions_[new_version] = key_bytes;
    active_key_version_ = new_version;
    std::memcpy(key_, key_bytes.data(), KEY_SIZE);
    SaveKeyRegistry();

    return active_key_version_;
}

std::vector<unsigned char> CryptoEngine::LoadSecretFile(const std::string& path) {
    if (path.empty() || !std::filesystem::exists(path)) {
        return {};
    }

    std::ifstream secret_file(path, std::ios::binary);
    if (!secret_file.is_open()) {
        throw std::runtime_error("Failed to open secret file: " + path);
    }

    std::vector<unsigned char> secret(KEY_SIZE);
    secret_file.read(reinterpret_cast<char*>(secret.data()), KEY_SIZE);
    if (secret_file.gcount() != KEY_SIZE) {
        throw std::runtime_error("Secret file must contain exactly 32 bytes: " + path);
    }
    return secret;
}

std::vector<unsigned char> CryptoEngine::DeriveSessionKey(const std::vector<std::string>& context_parts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DeriveSessionKeyWithSecret(payload_secret_, context_parts);
}

std::vector<unsigned char> CryptoEngine::DeriveSessionKeyWithSecret(
    const std::vector<unsigned char>& secret,
    const std::vector<std::string>& context_parts) {
    std::vector<unsigned char> out(32);
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;

    if (secret.size() != KEY_SIZE) {
        throw std::runtime_error("Payload secret must contain exactly 32 bytes");
    }

    std::string to_mac = "pqfl-session-v3";
    for (const auto& part : context_parts) {
        to_mac.append("|");
        to_mac.append(std::to_string(part.size()));
        to_mac.append(":");
        to_mac.append(part);
    }

    if (HMAC(EVP_sha256(),
             secret.data(),
             static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(to_mac.data()),
             to_mac.size(),
             mac,
             &mac_len) == nullptr ||
        mac_len < KEY_SIZE) {
        throw std::runtime_error("Failed to derive payload session key");
    }

    std::memcpy(out.data(), mac, KEY_SIZE);
    return out;
}

std::string CryptoEngine::EncryptWithKey(const unsigned char* key,
                                         const std::string& plaintext,
                                         std::vector<unsigned char>& iv_out,
                                         std::vector<unsigned char>& tag_out) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }

    iv_out.resize(IV_SIZE);
    if (RAND_bytes(iv_out.data(), IV_SIZE) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to generate IV");
    }

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv_out.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EncryptInit failed");
    }

    int outlen = 0;
    std::string ciphertext(plaintext.size() + EVP_CIPHER_block_size(EVP_aes_256_gcm()), '\0');

    if (EVP_EncryptUpdate(ctx,
                          reinterpret_cast<unsigned char*>(&ciphertext[0]),
                          &outlen,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          plaintext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EncryptUpdate failed");
    }

    int final_len = outlen;
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&ciphertext[final_len]), &outlen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EncryptFinal failed");
    }
    final_len += outlen;
    ciphertext.resize(final_len);

    tag_out.resize(TAG_SIZE);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag_out.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to get GCM tag");
    }

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext;
}

std::string CryptoEngine::DecryptWithKey(const unsigned char* key,
                                         const std::string& ciphertext,
                                         const std::vector<unsigned char>& iv,
                                         const std::vector<unsigned char>& tag) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("DecryptInit failed");
    }

    int outlen = 0;
    std::string plaintext(ciphertext.size(), '\0');

    if (EVP_DecryptUpdate(ctx,
                          reinterpret_cast<unsigned char*>(&plaintext[0]),
                          &outlen,
                          reinterpret_cast<const unsigned char*>(ciphertext.data()),
                          ciphertext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("DecryptUpdate failed");
    }

    int final_len = outlen;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                            const_cast<unsigned char*>(tag.data())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set expected GCM tag");
    }

    int ret = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&plaintext[final_len]), &outlen);
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) {
        throw std::runtime_error("Decryption failed: GCM tag mismatch (integrity violation)");
    }

    final_len += outlen;
    plaintext.resize(final_len);
    return plaintext;
}
