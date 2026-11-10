#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "login.h"
#include <stdexcept>
#include <windows.h>

#include <wincred.h>

void writeCredential(const QString &reference, const QString &user,
                     const QString &password) {
  auto bytes = password.toUtf8();
  if (bytes.isEmpty() || bytes.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
    throw std::runtime_error("Credential cannot be stored");
  CREDENTIALW c{};
  c.Type = CRED_TYPE_GENERIC;
  c.Persist = CRED_PERSIST_LOCAL_MACHINE;
  c.TargetName =
      const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(reference.utf16()));
  c.UserName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(user.utf16()));
  c.CredentialBlob = reinterpret_cast<LPBYTE>(bytes.data());
  c.CredentialBlobSize = DWORD(bytes.size());
  const bool ok = CredWriteW(&c, 0);
  SecureZeroMemory(bytes.data(), size_t(bytes.size()));
  if (!ok)
    throw std::runtime_error("CREDENTIALS_LOCKED");
}

QString readCredential(const QString &reference) {
  const auto value = findCredential(reference);
  if (!value)
    throw std::runtime_error("AUTH_REQUIRED");
  return *value;
}

std::optional<QString> findCredential(const QString &reference) {
  PCREDENTIALW c = nullptr;
  if (!CredReadW(reinterpret_cast<LPCWSTR>(reference.utf16()),
                 CRED_TYPE_GENERIC, 0, &c)) {
    if (GetLastError() == ERROR_NOT_FOUND)
      return std::nullopt;
    throw std::runtime_error("CREDENTIALS_LOCKED");
  }
  auto password = QString::fromUtf8(
      reinterpret_cast<const char *>(c->CredentialBlob), c->CredentialBlobSize);
  SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
  CredFree(c);
  return password;
}

void removeCredential(const QString &reference) {
  if (!CredDeleteW(reinterpret_cast<LPCWSTR>(reference.utf16()),
                   CRED_TYPE_GENERIC, 0) &&
      GetLastError() != ERROR_NOT_FOUND)
    throw std::runtime_error("Credential cleanup failed");
}
