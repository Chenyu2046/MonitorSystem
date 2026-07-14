#pragma once

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace monitor::runtime_config {

struct DatabaseConfig {
  std::string host;
  std::string user;
  std::string password;
  std::string database;
};

struct AuthorizationConfig {
  bool allow_insecure = false;
  std::vector<std::string> worker_sans;
  std::vector<std::string> query_sans;
};

inline bool ReadRequiredEnv(const char* name, std::string* value,
                            std::string* error) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || raw[0] == '\0') {
    *error = std::string("Missing required environment variable: ") + name;
    return false;
  }
  *value = raw;
  return true;
}

inline bool ReadFile(const std::string& path, std::string* value,
                     std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "Unable to read required TLS file: " + path;
    return false;
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  *value = contents.str();
  if (value->empty()) {
    *error = "TLS file is empty: " + path;
    return false;
  }
  return true;
}

inline bool AllowInsecureGrpc() {
  const char* value = std::getenv("MONITOR_ALLOW_INSECURE_GRPC");
  return value != nullptr && std::string(value) == "1";
}

inline bool LoadDatabaseConfig(DatabaseConfig* config, std::string* error) {
  return ReadRequiredEnv("MONITOR_DB_HOST", &config->host, error) &&
         ReadRequiredEnv("MONITOR_DB_USER", &config->user, error) &&
         ReadRequiredEnv("MONITOR_DB_PASSWORD", &config->password, error) &&
         ReadRequiredEnv("MONITOR_DB_NAME", &config->database, error);
}

inline std::vector<std::string> SplitCommaList(const std::string& value) {
  std::vector<std::string> values;
  std::istringstream input(value);
  std::string item;
  while (std::getline(input, item, ',')) {
    if (!item.empty()) values.push_back(item);
  }
  return values;
}

inline bool LoadAuthorizationConfig(AuthorizationConfig* config,
                                    std::string* error) {
  config->allow_insecure = AllowInsecureGrpc();
  if (config->allow_insecure) return true;

  std::string workers;
  std::string queries;
  if (!ReadRequiredEnv("MONITOR_WORKER_SAN_ALLOWLIST", &workers, error) ||
      !ReadRequiredEnv("MONITOR_QUERY_SAN_ALLOWLIST", &queries, error)) {
    return false;
  }
  config->worker_sans = SplitCommaList(workers);
  config->query_sans = SplitCommaList(queries);
  if (config->worker_sans.empty() || config->query_sans.empty()) {
    *error = "SAN allowlists must contain at least one identity";
    return false;
  }
  return true;
}

inline bool PeerHasAllowedSan(
    const grpc::ServerContext& context,
    const std::vector<std::string>& allowed_sans) {
  const auto auth_context = context.auth_context();
  if (!auth_context || !auth_context->IsPeerAuthenticated()) return false;
  const auto peer_sans =
      auth_context->FindPropertyValues("x509_subject_alternative_name");
  for (const auto& peer_san : peer_sans) {
    const std::string value(peer_san.data(), peer_san.size());
    for (const auto& allowed_san : allowed_sans) {
      if (value == allowed_san) return true;
    }
  }
  return false;
}

inline bool LoadTlsFiles(std::string* ca, std::string* cert, std::string* key,
                         std::string* error) {
  std::string ca_path;
  std::string cert_path;
  std::string key_path;
  return ReadRequiredEnv("MONITOR_GRPC_TLS_CA_FILE", &ca_path, error) &&
         ReadRequiredEnv("MONITOR_GRPC_TLS_CERT_FILE", &cert_path, error) &&
         ReadRequiredEnv("MONITOR_GRPC_TLS_KEY_FILE", &key_path, error) &&
         ReadFile(ca_path, ca, error) && ReadFile(cert_path, cert, error) &&
         ReadFile(key_path, key, error);
}

inline std::shared_ptr<grpc::ServerCredentials> BuildServerCredentials(
    std::string* error) {
  if (AllowInsecureGrpc()) {
    return grpc::InsecureServerCredentials();
  }

  std::string ca;
  std::string cert;
  std::string key;
  if (!LoadTlsFiles(&ca, &cert, &key, error)) {
    return nullptr;
  }

  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = ca;
  options.pem_key_cert_pairs.push_back({key, cert});
  options.client_certificate_request =
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  return grpc::SslServerCredentials(options);
}

inline std::shared_ptr<grpc::ChannelCredentials> BuildClientCredentials(
    std::string* error) {
  if (AllowInsecureGrpc()) {
    return grpc::InsecureChannelCredentials();
  }

  std::string ca;
  std::string cert;
  std::string key;
  if (!LoadTlsFiles(&ca, &cert, &key, error)) {
    return nullptr;
  }

  grpc::SslCredentialsOptions options;
  options.pem_root_certs = ca;
  options.pem_cert_chain = cert;
  options.pem_private_key = key;
  return grpc::SslCredentials(options);
}

}  // namespace monitor::runtime_config
