#ifndef IDENTITY_UTILS_HPP
#define IDENTITY_UTILS_HPP

#include <cstddef>
#include <string>
#include <vector>
#include <grpcpp/grpcpp.h>

std::string NormalizePeerIdentity(const std::string& value);
std::string GetPeerIdentity(grpc::ServerContext* context);
bool IsPeerAuthenticated(grpc::ServerContext* context);
bool HasAllowedPeerIdentity(grpc::ServerContext* context,
                            const std::vector<std::string>& allowed_identities);
std::string ValidateIdentifier(const std::string& value,
                               const std::string& field_name,
                               bool allow_empty = false,
                               size_t max_length = 128);

#endif // IDENTITY_UTILS_HPP
