#ifndef PQC_MASTER_THESIS_2026_P2PCREDENTIALGENERATOR_H
#define PQC_MASTER_THESIS_2026_P2PCREDENTIALGENERATOR_H

#include <botan/auto_rng.h>
#include <botan/data_src.h>
#include <botan/pk_algs.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/x509cert.h>
#include <botan/x509self.h>
#include <botan/rsa.h>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace Safira {

    inline std::filesystem::path GetSafiraDataDir() {
        const char* home = std::getenv("HOME");
        const std::filesystem::path base = (home && *home)
            ? std::filesystem::path(home) / ".safira"
            : std::filesystem::path(".safira");

        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        ::chmod(base.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
        return base;
    }

    inline void HardenFilePermissions(const std::filesystem::path& path) {
        ::chmod(path.string().c_str(), S_IRUSR | S_IWUSR);
    }

    struct P2PKeyMaterial {
        std::shared_ptr<Botan::Private_Key>      Key;
        std::shared_ptr<Botan::X509_Certificate> Cert;
    };

    enum class P2PKeyType {
        RSA_PSS,     // Classical — works now
        ML_DSA_65,   // Full post-quantum (Botan 3.6+)
    };

    inline P2PKeyMaterial GenerateP2PCredentials(
        P2PKeyType type = P2PKeyType::RSA_PSS,
        const std::string& cn = "safira-p2p")
    {
        auto rng = std::make_shared<Botan::AutoSeeded_RNG>();

        std::shared_ptr<Botan::Private_Key> key;
        std::string sig_padding;

        switch (type) {
            case P2PKeyType::RSA_PSS:
                key = std::make_shared<Botan::RSA_PrivateKey>(*rng, 2048);
                sig_padding = "SHA-256";
                break;

            case P2PKeyType::ML_DSA_65:
                key = Botan::create_private_key("ML-DSA", *rng, "ML-DSA-65");
                sig_padding = "";
                break;
        }

        // Constructor: X509_Cert_Options(common_name, validity_duration)
        Botan::X509_Cert_Options opts(cn, 3650 * 24 * 60 * 60);

        auto cert = std::make_shared<Botan::X509_Certificate>(
            Botan::X509::create_self_signed_cert(opts, *key, sig_padding, *rng));

        return { key, cert };
    }

    inline std::string SanitizeIdentityName(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (const char c : raw) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')
                out.push_back(c);
            else
                out.push_back('_');
        }
        if (out.empty())
            out = "default";
        return out;
    }

    inline P2PKeyMaterial GenerateOrLoadP2PCredentials(
        std::string_view identityName,
        P2PKeyType type = P2PKeyType::RSA_PSS)
    {
        const auto safeName = SanitizeIdentityName(identityName);
        const std::filesystem::path dir = GetSafiraDataDir() / "p2p_identities";
        const std::filesystem::path keyPath = dir / (safeName + ".key.pem");
        const std::filesystem::path certPath = dir / (safeName + ".cert.pem");

        std::filesystem::create_directories(dir);
        ::chmod(dir.string().c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
        auto rng = std::make_shared<Botan::AutoSeeded_RNG>();

        if (std::filesystem::exists(keyPath) && std::filesystem::exists(certPath)) {
            try {
                Botan::DataSource_Stream keySrc(keyPath.string(), true);
                auto keyUnique = Botan::PKCS8::load_key(keySrc);
                auto cert = std::make_shared<Botan::X509_Certificate>(certPath.string());
                if (keyUnique && cert) {
                    auto key = std::shared_ptr<Botan::Private_Key>(std::move(keyUnique));
                    return { key, cert };
                }
            } catch (...) {
                // Fall through to regeneration below.
            }
        }

        auto material = GenerateP2PCredentials(type, safeName);

        {
            std::ofstream keyOut(keyPath, std::ios::trunc);
            keyOut << Botan::PKCS8::PEM_encode(*material.Key);
            keyOut.flush();
        }
        HardenFilePermissions(keyPath);

        {
            std::ofstream certOut(certPath, std::ios::trunc);
            certOut << material.Cert->PEM_encode();
            certOut.flush();
        }
        HardenFilePermissions(certPath);

        return material;
    }

} // namespace Safira

#endif //PQC_MASTER_THESIS_2026_P2PCREDENTIALGENERATOR_H
