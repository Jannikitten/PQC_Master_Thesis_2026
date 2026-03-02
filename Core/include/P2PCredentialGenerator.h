#ifndef PQC_MASTER_THESIS_2026_P2PCREDENTIALGENERATOR_H
#define PQC_MASTER_THESIS_2026_P2PCREDENTIALGENERATOR_H

#include <botan/auto_rng.h>
#include <botan/pk_algs.h>
#include <botan/pk_keys.h>
#include <botan/x509cert.h>
#include <botan/x509self.h>
#include <botan/rsa.h>
#include <memory>
#include <string>

namespace Safira {

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

} // namespace Safira

#endif //PQC_MASTER_THESIS_2026_P2PCREDENTIALGENERATOR_H