#include "identity_utils.hpp"

#include <algorithm>
#include <cctype>

namespace {

std::string Trim(std::string value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string NormalizeIdentity(std::string value) {
    value = Trim(std::move(value));
    if (value.rfind("/CN=", 0) == 0) {
        return value.substr(4);
    }
    if (value.rfind("CN=", 0) == 0) {
        return value.substr(3);
    }
    return value;
}

bool IsAllowedIdentifierChar(unsigned char ch) {
    return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':';
}

} // namespace

std::string NormalizePeerIdentity(const std::string& value) {
    return NormalizeIdentity(value);
}

std::string GetPeerIdentity(grpc::ServerContext* context) {
    auto auth_context = context->auth_context();
    if (!auth_context) {
        return "";
    }

    auto peer_identity = auth_context->GetPeerIdentity();
    if (peer_identity.empty()) {
        return "";
    }

    return std::string(peer_identity[0].data(), peer_identity[0].size());
}

bool IsPeerAuthenticated(grpc::ServerContext* context) {
    auto auth_context = context->auth_context();
    return auth_context != nullptr && auth_context->IsPeerAuthenticated();
}

bool HasAllowedPeerIdentity(grpc::ServerContext* context,
                            const std::vector<std::string>& allowed_identities) {
    if (!IsPeerAuthenticated(context)) {
        return false;
    }

    std::string identity = GetPeerIdentity(context);
    std::string normalized_identity = NormalizePeerIdentity(identity);
    for (const auto& allowed : allowed_identities) {
        std::string normalized_allowed = NormalizePeerIdentity(allowed);
        if (!normalized_allowed.empty() &&
            (identity == normalized_allowed ||
             normalized_identity == normalized_allowed ||
             identity == allowed)) {
            return true;
        }
    }
    return false;
}

std::string ValidateIdentifier(const std::string& value,
                               const std::string& field_name,
                               bool allow_empty,
                               size_t max_length) {
    if (value.empty()) {
        return allow_empty ? "" : (field_name + " is required");
    }
    if (value.size() > max_length) {
        return field_name + " exceeds " + std::to_string(max_length) + " characters";
    }

    for (unsigned char ch : value) {
        if (!IsAllowedIdentifierChar(ch)) {
            return field_name + " contains unsupported characters";
        }
    }

    return "";
}
