#ifndef FEDERATED_HELPERS_HPP
#define FEDERATED_HELPERS_HPP

#include <string>
#include "identity_utils.hpp"

namespace pqfl_helpers {

inline std::string ValidateFederatedRequestIds(const std::string& tenant_id,
                                               const std::string& model_id,
                                               const std::string& client_id) {
    std::string error = ValidateIdentifier(tenant_id, "tenant_id", true);
    if (!error.empty()) return error;
    error = ValidateIdentifier(model_id, "model_id", true);
    if (!error.empty()) return error;
    return ValidateIdentifier(client_id, "client_id");
}

inline std::string NormalizeTenant(const std::string& tenant_id) {
    return tenant_id.empty() ? "default-tenant" : tenant_id;
}

inline std::string NormalizeModel(const std::string& model_id) {
    return model_id.empty() ? "default-model" : model_id;
}

inline std::string ScopeKey(const std::string& tenant_id, const std::string& model_id) {
    return NormalizeTenant(tenant_id) + "::" + NormalizeModel(model_id);
}

inline std::string RoundKey(const std::string& tenant_id, const std::string& model_id, int round) {
    return ScopeKey(tenant_id, model_id) + "::round:" + std::to_string(round);
}

inline std::string ClientKey(const std::string& tenant_id,
                             const std::string& model_id,
                             const std::string& client_id) {
    return ScopeKey(tenant_id, model_id) + "::client:" + client_id;
}

} // namespace pqfl_helpers

#endif // FEDERATED_HELPERS_HPP
