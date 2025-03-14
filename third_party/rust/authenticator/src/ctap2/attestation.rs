use super::utils::{from_slice_stream, read_be_u16, read_be_u32, read_byte};
use crate::crypto::COSEAlgorithm;
use crate::ctap2::commands::CommandError;
use crate::ctap2::server::RpIdHash;
use crate::ctap2::utils::serde_parse_err;
use crate::{crypto::COSEKey, errors::AuthenticatorError};
use serde::ser::{Error as SerError, SerializeMap, Serializer};
use serde::{
    de::{Error as SerdeError, MapAccess, Visitor},
    Deserialize, Deserializer, Serialize,
};
use serde_cbor;
use std::fmt;
use std::io::{Cursor, Read};

#[derive(Debug, PartialEq, Eq)]
pub enum HmacSecretResponse {
    /// This is returned by MakeCredential calls to display if CredRandom was
    /// successfully generated
    Confirmed(bool),
    /// This is returned by GetAssertion:
    /// AES256-CBC(shared_secret, HMAC-SHA265(CredRandom, salt1) || HMAC-SHA265(CredRandom, salt2))
    Secret(Vec<u8>),
}

impl Serialize for HmacSecretResponse {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            HmacSecretResponse::Confirmed(x) => serializer.serialize_bool(*x),
            HmacSecretResponse::Secret(x) => serializer.serialize_bytes(x),
        }
    }
}
impl<'de> Deserialize<'de> for HmacSecretResponse {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct HmacSecretResponseVisitor;

        impl<'de> Visitor<'de> for HmacSecretResponseVisitor {
            type Value = HmacSecretResponse;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array or a boolean")
            }

            fn visit_bytes<E>(self, v: &[u8]) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                Ok(HmacSecretResponse::Secret(v.to_vec()))
            }

            fn visit_bool<E>(self, v: bool) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                Ok(HmacSecretResponse::Confirmed(v))
            }
        }
        deserializer.deserialize_any(HmacSecretResponseVisitor)
    }
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct Extension {
    #[serde(rename = "pinMinLength", skip_serializing_if = "Option::is_none")]
    pub pin_min_length: Option<u64>,
    #[serde(rename = "hmac-secret", skip_serializing_if = "Option::is_none")]
    pub hmac_secret: Option<HmacSecretResponse>,
}

impl Extension {
    fn has_some(&self) -> bool {
        self.pin_min_length.is_some() || self.hmac_secret.is_some()
    }
}

#[derive(Serialize, PartialEq, Default, Eq, Clone)]
pub struct AAGuid(pub [u8; 16]);

impl AAGuid {
    pub fn from(src: &[u8]) -> Result<AAGuid, AuthenticatorError> {
        let mut payload = [0u8; 16];
        if src.len() != payload.len() {
            Err(AuthenticatorError::InternalError(String::from(
                "Failed to parse AAGuid",
            )))
        } else {
            payload.copy_from_slice(src);
            Ok(AAGuid(payload))
        }
    }
}

impl fmt::Debug for AAGuid {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(
            f,
            "AAGuid({:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x})",
            self.0[0],
            self.0[1],
            self.0[2],
            self.0[3],
            self.0[4],
            self.0[5],
            self.0[6],
            self.0[7],
            self.0[8],
            self.0[9],
            self.0[10],
            self.0[11],
            self.0[12],
            self.0[13],
            self.0[14],
            self.0[15]
        )
    }
}

impl<'de> Deserialize<'de> for AAGuid {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct AAGuidVisitor;

        impl<'de> Visitor<'de> for AAGuidVisitor {
            type Value = AAGuid;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array")
            }

            fn visit_bytes<E>(self, v: &[u8]) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                let mut buf = [0u8; 16];
                if v.len() != buf.len() {
                    return Err(E::invalid_length(v.len(), &"16"));
                }

                buf.copy_from_slice(v);

                Ok(AAGuid(buf))
            }
        }

        deserializer.deserialize_bytes(AAGuidVisitor)
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct AttestedCredentialData {
    pub aaguid: AAGuid,
    pub credential_id: Vec<u8>,
    pub credential_public_key: COSEKey,
}

fn parse_attested_cred_data<R: Read, E: SerdeError>(
    data: &mut R,
) -> Result<AttestedCredentialData, E> {
    let mut aaguid_raw = [0u8; 16];
    data.read_exact(&mut aaguid_raw)
        .map_err(|_| serde_parse_err("AAGuid"))?;
    let aaguid = AAGuid(aaguid_raw);
    let cred_len = read_be_u16(data)?;
    let mut credential_id = vec![0u8; cred_len as usize];
    data.read_exact(&mut credential_id)
        .map_err(|_| serde_parse_err("CredentialId"))?;
    let credential_public_key = from_slice_stream(data)?;
    Ok(AttestedCredentialData {
        aaguid,
        credential_id,
        credential_public_key,
    })
}

bitflags! {
    // Defining an exhaustive list of flags here ensures that `from_bits_truncate` is lossless and
    // that `from_bits` never returns None.
    pub struct AuthenticatorDataFlags: u8 {
        const USER_PRESENT = 0x01;
        const RESERVED_1 = 0x02;
        const USER_VERIFIED = 0x04;
        const RESERVED_3 = 0x08;
        const RESERVED_4 = 0x10;
        const RESERVED_5 = 0x20;
        const ATTESTED = 0x40;
        const EXTENSION_DATA = 0x80;
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct AuthenticatorData {
    pub rp_id_hash: RpIdHash,
    pub flags: AuthenticatorDataFlags,
    pub counter: u32,
    pub credential_data: Option<AttestedCredentialData>,
    pub extensions: Extension,
}

impl<'de> Deserialize<'de> for AuthenticatorData {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct AuthenticatorDataVisitor;

        impl<'de> Visitor<'de> for AuthenticatorDataVisitor {
            type Value = AuthenticatorData;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array")
            }

            fn visit_bytes<E>(self, input: &[u8]) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                let mut cursor = Cursor::new(input);
                let mut rp_id_hash_raw = [0u8; 32];
                cursor
                    .read_exact(&mut rp_id_hash_raw)
                    .map_err(|_| serde_parse_err("32 bytes"))?;
                let rp_id_hash = RpIdHash(rp_id_hash_raw);

                // preserve the flags, even if some reserved values are set.
                let flags = AuthenticatorDataFlags::from_bits_truncate(read_byte(&mut cursor)?);
                let counter = read_be_u32(&mut cursor)?;
                let mut credential_data = None;
                if flags.contains(AuthenticatorDataFlags::ATTESTED) {
                    credential_data = Some(parse_attested_cred_data(&mut cursor)?);
                }

                let extensions = if flags.contains(AuthenticatorDataFlags::EXTENSION_DATA) {
                    from_slice_stream(&mut cursor)?
                } else {
                    Default::default()
                };

                // TODO(baloo): we should check for end of buffer and raise a parse
                //              parse error if data is still in the buffer
                Ok(AuthenticatorData {
                    rp_id_hash,
                    flags,
                    counter,
                    credential_data,
                    extensions,
                })
            }
        }

        deserializer.deserialize_bytes(AuthenticatorDataVisitor)
    }
}

impl AuthenticatorData {
    // see https://www.w3.org/TR/webauthn-2/#sctn-authenticator-data
    // Authenticator Data
    //                   Name Length (in bytes)
    //               rpIdHash 32
    //                  flags 1
    //              signCount 4
    // attestedCredentialData variable (if present)
    //             extensions variable (if present)
    pub fn to_vec(&self) -> Result<Vec<u8>, AuthenticatorError> {
        let mut data = Vec::new();
        data.extend(self.rp_id_hash.0); // (1) "rpIDHash", len=32
        data.extend([self.flags.bits()]); // (2) "flags", len=1 (u8)
        data.extend(self.counter.to_be_bytes()); // (3) "signCount", len=4, 32-bit unsigned big-endian integer.

        // TODO(MS): Here flags=AT needs to be set, but this data comes from the security device
        //           and we (probably?) need to just trust the device to set the right flags
        if let Some(cred) = &self.credential_data {
            // see https://www.w3.org/TR/webauthn-2/#sctn-attested-credential-data
            // Attested Credential Data
            //                Name Length (in bytes)
            //              aaguid 16
            //  credentialIdLength 2
            //        credentialId L
            // credentialPublicKey variable
            data.extend(cred.aaguid.0); // (1) "aaguid", len=16
            data.extend((cred.credential_id.len() as u16).to_be_bytes()); // (2) "credentialIdLength", len=2, 16-bit unsigned big-endian integer
            data.extend(&cred.credential_id); // (3) "credentialId", len= see (2)
            data.extend(
                // (4) "credentialPublicKey", len=variable
                &serde_cbor::to_vec(&cred.credential_public_key)
                    .map_err(CommandError::Serializing)?,
            );
        }
        // TODO(MS): Here flags=ED needs to be set, but this data comes from the security device
        //           and we (probably?) need to just trust the device to set the right flags
        if self.extensions.has_some() {
            data.extend(
                // (5) "extensions", len=variable
                &serde_cbor::to_vec(&self.extensions).map_err(CommandError::Serializing)?,
            );
        }
        Ok(data)
    }
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
/// x509 encoded attestation certificate
pub struct AttestationCertificate(#[serde(with = "serde_bytes")] pub Vec<u8>);

impl AsRef<[u8]> for AttestationCertificate {
    fn as_ref(&self) -> &[u8] {
        self.0.as_ref()
    }
}

#[derive(Serialize, Deserialize, PartialEq, Eq)]
pub struct Signature(#[serde(with = "serde_bytes")] pub Vec<u8>);

impl fmt::Debug for Signature {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let value = base64::encode_config(&self.0, base64::URL_SAFE_NO_PAD);
        write!(f, "Signature({value})")
    }
}

impl AsRef<[u8]> for Signature {
    fn as_ref(&self) -> &[u8] {
        self.0.as_ref()
    }
}

impl From<&[u8]> for Signature {
    fn from(sig: &[u8]) -> Signature {
        Signature(sig.to_vec())
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum AttestationStatement {
    None,
    Packed(AttestationStatementPacked),
    // TODO(baloo): there is a couple other options than None and Packed:
    //              https://w3c.github.io/webauthn/#generating-an-attestation-object
    //              https://w3c.github.io/webauthn/#defined-attestation-formats
    //TPM,
    //AndroidKey,
    //AndroidSafetyNet,
    FidoU2F(AttestationStatementFidoU2F),
}

// Not all crypto-backends currently provide "crypto::verify()", so we do not implement it yet.
// Also not sure, if we really need it. Would be a sanity-check only, to verify the signature is valid,
// before sendig it out.
// impl AttestationStatement {
//     pub fn verify(&self, data: &[u8]) -> Result<bool, AuthenticatorError> {
//         match self {
//             AttestationStatement::None => Ok(true),
//             AttestationStatement::Unparsed(_) => Err(AuthenticatorError::Custom(
//                 "Unparsed attestation object can't be used to verify signature.".to_string(),
//             )),
//             AttestationStatement::FidoU2F(att) => {
//                 let res = crypto::verify(
//                     crypto::SignatureAlgorithm::ES256,
//                     &att.attestation_cert[0].as_ref(),
//                     att.sig.as_ref(),
//                     data,
//                 )?;
//                 Ok(res)
//             }
//             AttestationStatement::Packed(att) => {
//                 if att.alg != Alg::ES256 {
//                     return Err(AuthenticatorError::Custom(
//                         "Verification only supported for ES256".to_string(),
//                     ));
//                 }
//                 let res = crypto::verify(
//                     crypto::SignatureAlgorithm::ES256,
//                     att.attestation_cert[0].as_ref(),
//                     att.sig.as_ref(),
//                     data,
//                 )?;
//                 Ok(res)
//             }
//         }
//     }
// }

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
// See https://www.w3.org/TR/webauthn-2/#sctn-fido-u2f-attestation
// u2fStmtFormat = {
//                     x5c: [ attestnCert: bytes ],
//                     sig: bytes
//                 }
pub struct AttestationStatementFidoU2F {
    /// Certificate chain in x509 format
    #[serde(rename = "x5c")]
    pub attestation_cert: Vec<AttestationCertificate>, // (1) "x5c"
    pub sig: Signature, // (2) "sig"
}

impl AttestationStatementFidoU2F {
    pub fn new(cert: &[u8], signature: &[u8]) -> Self {
        AttestationStatementFidoU2F {
            attestation_cert: vec![AttestationCertificate(Vec::from(cert))],
            sig: Signature::from(signature),
        }
    }
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
// https://www.w3.org/TR/webauthn-2/#sctn-packed-attestation
// packedStmtFormat = {
//                       alg: COSEAlgorithmIdentifier,
//                       sig: bytes,
//                       x5c: [ attestnCert: bytes, * (caCert: bytes) ]
//                   } //
//                   {
//                       alg: COSEAlgorithmIdentifier
//                       sig: bytes,
//                   }
pub struct AttestationStatementPacked {
    pub alg: COSEAlgorithm, // (1) "alg"
    pub sig: Signature,     // (2) "sig"
    /// Certificate chain in x509 format
    #[serde(rename = "x5c", skip_serializing_if = "Vec::is_empty", default)]
    pub attestation_cert: Vec<AttestationCertificate>, // (3) "x5c"
}

#[derive(Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
enum AttestationFormat {
    #[serde(rename = "fido-u2f")]
    FidoU2F,
    Packed,
    None,
    // TOOD(baloo): only packed is implemented for now, see spec:
    //              https://www.w3.org/TR/webauthn/#defined-attestation-formats
    //TPM,
    //AndroidKey,
    //AndroidSafetyNet,
}

#[derive(Debug, PartialEq, Eq)]
pub struct AttestationObject {
    pub auth_data: AuthenticatorData,
    pub att_statement: AttestationStatement,
}

impl<'de> Deserialize<'de> for AttestationObject {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct AttestationObjectVisitor;

        impl<'de> Visitor<'de> for AttestationObjectVisitor {
            type Value = AttestationObject;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a cbor map")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut format: Option<AttestationFormat> = None;
                let mut auth_data = None;
                let mut att_statement = None;

                while let Some(key) = map.next_key()? {
                    match key {
                        // Spec for CTAP 2.0 is wrong and fmt should be numbered 1, and auth_data 2:
                        // https://fidoalliance.org/specs/fido-v2.0-ps-20190130/fido-client-to-authenticator-protocol-v2.0-ps-20190130.html#authenticatorMakeCredential
                        // Corrected in CTAP 2.1 and Webauthn spec
                        1 => {
                            if format.is_some() {
                                return Err(SerdeError::duplicate_field("fmt"));
                            }
                            format = Some(map.next_value()?);
                        }
                        2 => {
                            if auth_data.is_some() {
                                return Err(SerdeError::duplicate_field("auth_data"));
                            }
                            auth_data = Some(map.next_value()?);
                        }
                        3 => {
                            let format = format
                                .as_ref()
                                .ok_or_else(|| SerdeError::missing_field("fmt"))?;
                            if att_statement.is_some() {
                                return Err(SerdeError::duplicate_field("att_statement"));
                            }
                            match format {
                                // This should not actually happen, but ...
                                AttestationFormat::None => {
                                    att_statement = Some(AttestationStatement::None);
                                }
                                AttestationFormat::Packed => {
                                    att_statement =
                                        Some(AttestationStatement::Packed(map.next_value()?));
                                }
                                AttestationFormat::FidoU2F => {
                                    att_statement =
                                        Some(AttestationStatement::FidoU2F(map.next_value()?));
                                }
                            }
                        }
                        k => return Err(M::Error::custom(format!("unexpected key: {k:?}"))),
                    }
                }

                let auth_data =
                    auth_data.ok_or_else(|| M::Error::custom("found no auth_data".to_string()))?;
                let att_statement = att_statement.unwrap_or(AttestationStatement::None);

                Ok(AttestationObject {
                    auth_data,
                    att_statement,
                })
            }
        }

        deserializer.deserialize_bytes(AttestationObjectVisitor)
    }
}

impl Serialize for AttestationObject {
    /// Serialize can be used to repackage the CBOR answer we get from the token using CTAP-format
    /// to webauthn-format (string-keys like "authData" instead of numbers). Yes, the specs are weird.
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        let map_len = 3;
        let mut map = serializer.serialize_map(Some(map_len))?;

        // CTAP2 canonical CBOR order for these entries is ("fmt", "attStmt", "authData")
        // as strings are sorted by length and then lexically.
        // see https://www.w3.org/TR/webauthn-2/#attestation-object
        match self.att_statement {
            AttestationStatement::None => {
                // Even with Att None, an empty map is returned in the cbor!
                map.serialize_entry(&"fmt", &"none")?; // (1) "fmt"
                let v = serde_cbor::Value::Map(std::collections::BTreeMap::new());
                map.serialize_entry(&"attStmt", &v)?; // (2) "attStmt"
            }
            AttestationStatement::Packed(ref v) => {
                map.serialize_entry(&"fmt", &"packed")?; // (1) "fmt"
                map.serialize_entry(&"attStmt", v)?; // (2) "attStmt"
            }
            AttestationStatement::FidoU2F(ref v) => {
                map.serialize_entry(&"fmt", &"fido-u2f")?; // (1) "fmt"
                map.serialize_entry(&"attStmt", v)?; // (2) "attStmt"
            }
        }

        let auth_data = self
            .auth_data
            .to_vec()
            .map(serde_cbor::Value::Bytes)
            .map_err(|_| SerError::custom("Failed to serialize auth_data"))?;
        map.serialize_entry(&"authData", &auth_data)?; // (3) "authData"
        map.end()
    }
}

#[cfg(test)]
mod test {
    use super::super::utils::from_slice_stream;
    use super::*;
    use serde_cbor::from_slice;

    const SAMPLE_ATTESTATION: [u8; 1006] = [
        0xa3, 0x1, 0x66, 0x70, 0x61, 0x63, 0x6b, 0x65, 0x64, 0x2, 0x58, 0xc4, 0x49, 0x96, 0xd,
        0xe5, 0x88, 0xe, 0x8c, 0x68, 0x74, 0x34, 0x17, 0xf, 0x64, 0x76, 0x60, 0x5b, 0x8f, 0xe4,
        0xae, 0xb9, 0xa2, 0x86, 0x32, 0xc7, 0x99, 0x5c, 0xf3, 0xba, 0x83, 0x1d, 0x97, 0x63, 0x41,
        0x0, 0x0, 0x0, 0x7, 0xcb, 0x69, 0x48, 0x1e, 0x8f, 0xf7, 0x40, 0x39, 0x93, 0xec, 0xa, 0x27,
        0x29, 0xa1, 0x54, 0xa8, 0x0, 0x40, 0xc3, 0xcf, 0x1, 0x3b, 0xc6, 0x26, 0x93, 0x28, 0xfb,
        0x7f, 0xa9, 0x76, 0xef, 0xa8, 0x4b, 0x66, 0x71, 0xad, 0xa9, 0x64, 0xea, 0xcb, 0x58, 0x76,
        0x54, 0x51, 0xa, 0xc8, 0x86, 0x4f, 0xbb, 0x53, 0x2d, 0xfb, 0x2, 0xfc, 0xdc, 0xa9, 0x84,
        0xc2, 0x5c, 0x67, 0x8a, 0x3a, 0xab, 0x57, 0xf3, 0x71, 0x77, 0xd3, 0xd4, 0x41, 0x64, 0x1,
        0x50, 0xca, 0x6c, 0x42, 0x73, 0x1c, 0x42, 0xcb, 0x81, 0xba, 0xa5, 0x1, 0x2, 0x3, 0x26,
        0x20, 0x1, 0x21, 0x58, 0x20, 0x9, 0x2e, 0x34, 0xfe, 0xa7, 0xd7, 0x32, 0xc8, 0xae, 0x4c,
        0xf6, 0x96, 0xbe, 0x7a, 0x12, 0xdc, 0x29, 0xd5, 0xf1, 0xd3, 0xf1, 0x55, 0x4d, 0xdc, 0x87,
        0xc4, 0xc, 0x9b, 0xd0, 0x17, 0xba, 0xf, 0x22, 0x58, 0x20, 0xc9, 0xf0, 0x97, 0x33, 0x55,
        0x36, 0x58, 0xd9, 0xdb, 0x76, 0xf5, 0xef, 0x95, 0xcf, 0x8a, 0xc7, 0xfc, 0xc1, 0xb6, 0x81,
        0x25, 0x5f, 0x94, 0x6b, 0x62, 0x13, 0x7d, 0xd0, 0xc4, 0x86, 0x53, 0xdb, 0x3, 0xa3, 0x63,
        0x61, 0x6c, 0x67, 0x26, 0x63, 0x73, 0x69, 0x67, 0x58, 0x48, 0x30, 0x46, 0x2, 0x21, 0x0,
        0xac, 0x2a, 0x78, 0xa8, 0xaf, 0x18, 0x80, 0x39, 0x73, 0x8d, 0x3, 0x5e, 0x4, 0x4d, 0x94,
        0x4f, 0x3f, 0x57, 0xce, 0x88, 0x41, 0xfa, 0x81, 0x50, 0x40, 0xb6, 0xd1, 0x95, 0xb5, 0xeb,
        0xe4, 0x6f, 0x2, 0x21, 0x0, 0x8f, 0xf4, 0x15, 0xc9, 0xb3, 0x6d, 0x1c, 0xd, 0x4c, 0xa3,
        0xcf, 0x99, 0x8a, 0x46, 0xd4, 0x4c, 0x8b, 0x5c, 0x26, 0x3f, 0xdf, 0x22, 0x6c, 0x9b, 0x23,
        0x83, 0x8b, 0x69, 0x47, 0x67, 0x48, 0x45, 0x63, 0x78, 0x35, 0x63, 0x81, 0x59, 0x2, 0xc1,
        0x30, 0x82, 0x2, 0xbd, 0x30, 0x82, 0x1, 0xa5, 0xa0, 0x3, 0x2, 0x1, 0x2, 0x2, 0x4, 0x18,
        0xac, 0x46, 0xc0, 0x30, 0xd, 0x6, 0x9, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0xd, 0x1, 0x1, 0xb,
        0x5, 0x0, 0x30, 0x2e, 0x31, 0x2c, 0x30, 0x2a, 0x6, 0x3, 0x55, 0x4, 0x3, 0x13, 0x23, 0x59,
        0x75, 0x62, 0x69, 0x63, 0x6f, 0x20, 0x55, 0x32, 0x46, 0x20, 0x52, 0x6f, 0x6f, 0x74, 0x20,
        0x43, 0x41, 0x20, 0x53, 0x65, 0x72, 0x69, 0x61, 0x6c, 0x20, 0x34, 0x35, 0x37, 0x32, 0x30,
        0x30, 0x36, 0x33, 0x31, 0x30, 0x20, 0x17, 0xd, 0x31, 0x34, 0x30, 0x38, 0x30, 0x31, 0x30,
        0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x18, 0xf, 0x32, 0x30, 0x35, 0x30, 0x30, 0x39, 0x30,
        0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x30, 0x6e, 0x31, 0xb, 0x30, 0x9, 0x6, 0x3,
        0x55, 0x4, 0x6, 0x13, 0x2, 0x53, 0x45, 0x31, 0x12, 0x30, 0x10, 0x6, 0x3, 0x55, 0x4, 0xa,
        0xc, 0x9, 0x59, 0x75, 0x62, 0x69, 0x63, 0x6f, 0x20, 0x41, 0x42, 0x31, 0x22, 0x30, 0x20,
        0x6, 0x3, 0x55, 0x4, 0xb, 0xc, 0x19, 0x41, 0x75, 0x74, 0x68, 0x65, 0x6e, 0x74, 0x69, 0x63,
        0x61, 0x74, 0x6f, 0x72, 0x20, 0x41, 0x74, 0x74, 0x65, 0x73, 0x74, 0x61, 0x74, 0x69, 0x6f,
        0x6e, 0x31, 0x27, 0x30, 0x25, 0x6, 0x3, 0x55, 0x4, 0x3, 0xc, 0x1e, 0x59, 0x75, 0x62, 0x69,
        0x63, 0x6f, 0x20, 0x55, 0x32, 0x46, 0x20, 0x45, 0x45, 0x20, 0x53, 0x65, 0x72, 0x69, 0x61,
        0x6c, 0x20, 0x34, 0x31, 0x33, 0x39, 0x34, 0x33, 0x34, 0x38, 0x38, 0x30, 0x59, 0x30, 0x13,
        0x6, 0x7, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x2, 0x1, 0x6, 0x8, 0x2a, 0x86, 0x48, 0xce, 0x3d,
        0x3, 0x1, 0x7, 0x3, 0x42, 0x0, 0x4, 0x79, 0xea, 0x3b, 0x2c, 0x7c, 0x49, 0x70, 0x10, 0x62,
        0x23, 0xc, 0xd2, 0x3f, 0xeb, 0x60, 0xe5, 0x29, 0x31, 0x71, 0xd4, 0x83, 0xf1, 0x0, 0xbe,
        0x85, 0x9d, 0x6b, 0xf, 0x83, 0x97, 0x3, 0x1, 0xb5, 0x46, 0xcd, 0xd4, 0x6e, 0xcf, 0xca,
        0xe3, 0xe3, 0xf3, 0xf, 0x81, 0xe9, 0xed, 0x62, 0xbd, 0x26, 0x8d, 0x4c, 0x1e, 0xbd, 0x37,
        0xb3, 0xbc, 0xbe, 0x92, 0xa8, 0xc2, 0xae, 0xeb, 0x4e, 0x3a, 0xa3, 0x6c, 0x30, 0x6a, 0x30,
        0x22, 0x6, 0x9, 0x2b, 0x6, 0x1, 0x4, 0x1, 0x82, 0xc4, 0xa, 0x2, 0x4, 0x15, 0x31, 0x2e,
        0x33, 0x2e, 0x36, 0x2e, 0x31, 0x2e, 0x34, 0x2e, 0x31, 0x2e, 0x34, 0x31, 0x34, 0x38, 0x32,
        0x2e, 0x31, 0x2e, 0x37, 0x30, 0x13, 0x6, 0xb, 0x2b, 0x6, 0x1, 0x4, 0x1, 0x82, 0xe5, 0x1c,
        0x2, 0x1, 0x1, 0x4, 0x4, 0x3, 0x2, 0x5, 0x20, 0x30, 0x21, 0x6, 0xb, 0x2b, 0x6, 0x1, 0x4,
        0x1, 0x82, 0xe5, 0x1c, 0x1, 0x1, 0x4, 0x4, 0x12, 0x4, 0x10, 0xcb, 0x69, 0x48, 0x1e, 0x8f,
        0xf7, 0x40, 0x39, 0x93, 0xec, 0xa, 0x27, 0x29, 0xa1, 0x54, 0xa8, 0x30, 0xc, 0x6, 0x3, 0x55,
        0x1d, 0x13, 0x1, 0x1, 0xff, 0x4, 0x2, 0x30, 0x0, 0x30, 0xd, 0x6, 0x9, 0x2a, 0x86, 0x48,
        0x86, 0xf7, 0xd, 0x1, 0x1, 0xb, 0x5, 0x0, 0x3, 0x82, 0x1, 0x1, 0x0, 0x97, 0x9d, 0x3, 0x97,
        0xd8, 0x60, 0xf8, 0x2e, 0xe1, 0x5d, 0x31, 0x1c, 0x79, 0x6e, 0xba, 0xfb, 0x22, 0xfa, 0xa7,
        0xe0, 0x84, 0xd9, 0xba, 0xb4, 0xc6, 0x1b, 0xbb, 0x57, 0xf3, 0xe6, 0xb4, 0xc1, 0x8a, 0x48,
        0x37, 0xb8, 0x5c, 0x3c, 0x4e, 0xdb, 0xe4, 0x83, 0x43, 0xf4, 0xd6, 0xa5, 0xd9, 0xb1, 0xce,
        0xda, 0x8a, 0xe1, 0xfe, 0xd4, 0x91, 0x29, 0x21, 0x73, 0x5, 0x8e, 0x5e, 0xe1, 0xcb, 0xdd,
        0x6b, 0xda, 0xc0, 0x75, 0x57, 0xc6, 0xa0, 0xe8, 0xd3, 0x68, 0x25, 0xba, 0x15, 0x9e, 0x7f,
        0xb5, 0xad, 0x8c, 0xda, 0xf8, 0x4, 0x86, 0x8c, 0xf9, 0xe, 0x8f, 0x1f, 0x8a, 0xea, 0x17,
        0xc0, 0x16, 0xb5, 0x5c, 0x2a, 0x7a, 0xd4, 0x97, 0xc8, 0x94, 0xfb, 0x71, 0xd7, 0x53, 0xd7,
        0x9b, 0x9a, 0x48, 0x4b, 0x6c, 0x37, 0x6d, 0x72, 0x3b, 0x99, 0x8d, 0x2e, 0x1d, 0x43, 0x6,
        0xbf, 0x10, 0x33, 0xb5, 0xae, 0xf8, 0xcc, 0xa5, 0xcb, 0xb2, 0x56, 0x8b, 0x69, 0x24, 0x22,
        0x6d, 0x22, 0xa3, 0x58, 0xab, 0x7d, 0x87, 0xe4, 0xac, 0x5f, 0x2e, 0x9, 0x1a, 0xa7, 0x15,
        0x79, 0xf3, 0xa5, 0x69, 0x9, 0x49, 0x7d, 0x72, 0xf5, 0x4e, 0x6, 0xba, 0xc1, 0xc3, 0xb4,
        0x41, 0x3b, 0xba, 0x5e, 0xaf, 0x94, 0xc3, 0xb6, 0x4f, 0x34, 0xf9, 0xeb, 0xa4, 0x1a, 0xcb,
        0x6a, 0xe2, 0x83, 0x77, 0x6d, 0x36, 0x46, 0x53, 0x78, 0x48, 0xfe, 0xe8, 0x84, 0xbd, 0xdd,
        0xf5, 0xb1, 0xba, 0x57, 0x98, 0x54, 0xcf, 0xfd, 0xce, 0xba, 0xc3, 0x44, 0x5, 0x95, 0x27,
        0xe5, 0x6d, 0xd5, 0x98, 0xf8, 0xf5, 0x66, 0x71, 0x5a, 0xbe, 0x43, 0x1, 0xdd, 0x19, 0x11,
        0x30, 0xe6, 0xb9, 0xf0, 0xc6, 0x40, 0x39, 0x12, 0x53, 0xe2, 0x29, 0x80, 0x3f, 0x3a, 0xef,
        0x27, 0x4b, 0xed, 0xbf, 0xde, 0x3f, 0xcb, 0xbd, 0x42, 0xea, 0xd6, 0x79,
    ];

    const SAMPLE_CERT_CHAIN: [u8; 709] = [
        0x81, 0x59, 0x2, 0xc1, 0x30, 0x82, 0x2, 0xbd, 0x30, 0x82, 0x1, 0xa5, 0xa0, 0x3, 0x2, 0x1,
        0x2, 0x2, 0x4, 0x18, 0xac, 0x46, 0xc0, 0x30, 0xd, 0x6, 0x9, 0x2a, 0x86, 0x48, 0x86, 0xf7,
        0xd, 0x1, 0x1, 0xb, 0x5, 0x0, 0x30, 0x2e, 0x31, 0x2c, 0x30, 0x2a, 0x6, 0x3, 0x55, 0x4, 0x3,
        0x13, 0x23, 0x59, 0x75, 0x62, 0x69, 0x63, 0x6f, 0x20, 0x55, 0x32, 0x46, 0x20, 0x52, 0x6f,
        0x6f, 0x74, 0x20, 0x43, 0x41, 0x20, 0x53, 0x65, 0x72, 0x69, 0x61, 0x6c, 0x20, 0x34, 0x35,
        0x37, 0x32, 0x30, 0x30, 0x36, 0x33, 0x31, 0x30, 0x20, 0x17, 0xd, 0x31, 0x34, 0x30, 0x38,
        0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x18, 0xf, 0x32, 0x30, 0x35, 0x30,
        0x30, 0x39, 0x30, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x30, 0x6e, 0x31, 0xb,
        0x30, 0x9, 0x6, 0x3, 0x55, 0x4, 0x6, 0x13, 0x2, 0x53, 0x45, 0x31, 0x12, 0x30, 0x10, 0x6,
        0x3, 0x55, 0x4, 0xa, 0xc, 0x9, 0x59, 0x75, 0x62, 0x69, 0x63, 0x6f, 0x20, 0x41, 0x42, 0x31,
        0x22, 0x30, 0x20, 0x6, 0x3, 0x55, 0x4, 0xb, 0xc, 0x19, 0x41, 0x75, 0x74, 0x68, 0x65, 0x6e,
        0x74, 0x69, 0x63, 0x61, 0x74, 0x6f, 0x72, 0x20, 0x41, 0x74, 0x74, 0x65, 0x73, 0x74, 0x61,
        0x74, 0x69, 0x6f, 0x6e, 0x31, 0x27, 0x30, 0x25, 0x6, 0x3, 0x55, 0x4, 0x3, 0xc, 0x1e, 0x59,
        0x75, 0x62, 0x69, 0x63, 0x6f, 0x20, 0x55, 0x32, 0x46, 0x20, 0x45, 0x45, 0x20, 0x53, 0x65,
        0x72, 0x69, 0x61, 0x6c, 0x20, 0x34, 0x31, 0x33, 0x39, 0x34, 0x33, 0x34, 0x38, 0x38, 0x30,
        0x59, 0x30, 0x13, 0x6, 0x7, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x2, 0x1, 0x6, 0x8, 0x2a, 0x86,
        0x48, 0xce, 0x3d, 0x3, 0x1, 0x7, 0x3, 0x42, 0x0, 0x4, 0x79, 0xea, 0x3b, 0x2c, 0x7c, 0x49,
        0x70, 0x10, 0x62, 0x23, 0xc, 0xd2, 0x3f, 0xeb, 0x60, 0xe5, 0x29, 0x31, 0x71, 0xd4, 0x83,
        0xf1, 0x0, 0xbe, 0x85, 0x9d, 0x6b, 0xf, 0x83, 0x97, 0x3, 0x1, 0xb5, 0x46, 0xcd, 0xd4, 0x6e,
        0xcf, 0xca, 0xe3, 0xe3, 0xf3, 0xf, 0x81, 0xe9, 0xed, 0x62, 0xbd, 0x26, 0x8d, 0x4c, 0x1e,
        0xbd, 0x37, 0xb3, 0xbc, 0xbe, 0x92, 0xa8, 0xc2, 0xae, 0xeb, 0x4e, 0x3a, 0xa3, 0x6c, 0x30,
        0x6a, 0x30, 0x22, 0x6, 0x9, 0x2b, 0x6, 0x1, 0x4, 0x1, 0x82, 0xc4, 0xa, 0x2, 0x4, 0x15,
        0x31, 0x2e, 0x33, 0x2e, 0x36, 0x2e, 0x31, 0x2e, 0x34, 0x2e, 0x31, 0x2e, 0x34, 0x31, 0x34,
        0x38, 0x32, 0x2e, 0x31, 0x2e, 0x37, 0x30, 0x13, 0x6, 0xb, 0x2b, 0x6, 0x1, 0x4, 0x1, 0x82,
        0xe5, 0x1c, 0x2, 0x1, 0x1, 0x4, 0x4, 0x3, 0x2, 0x5, 0x20, 0x30, 0x21, 0x6, 0xb, 0x2b, 0x6,
        0x1, 0x4, 0x1, 0x82, 0xe5, 0x1c, 0x1, 0x1, 0x4, 0x4, 0x12, 0x4, 0x10, 0xcb, 0x69, 0x48,
        0x1e, 0x8f, 0xf7, 0x40, 0x39, 0x93, 0xec, 0xa, 0x27, 0x29, 0xa1, 0x54, 0xa8, 0x30, 0xc,
        0x6, 0x3, 0x55, 0x1d, 0x13, 0x1, 0x1, 0xff, 0x4, 0x2, 0x30, 0x0, 0x30, 0xd, 0x6, 0x9, 0x2a,
        0x86, 0x48, 0x86, 0xf7, 0xd, 0x1, 0x1, 0xb, 0x5, 0x0, 0x3, 0x82, 0x1, 0x1, 0x0, 0x97, 0x9d,
        0x3, 0x97, 0xd8, 0x60, 0xf8, 0x2e, 0xe1, 0x5d, 0x31, 0x1c, 0x79, 0x6e, 0xba, 0xfb, 0x22,
        0xfa, 0xa7, 0xe0, 0x84, 0xd9, 0xba, 0xb4, 0xc6, 0x1b, 0xbb, 0x57, 0xf3, 0xe6, 0xb4, 0xc1,
        0x8a, 0x48, 0x37, 0xb8, 0x5c, 0x3c, 0x4e, 0xdb, 0xe4, 0x83, 0x43, 0xf4, 0xd6, 0xa5, 0xd9,
        0xb1, 0xce, 0xda, 0x8a, 0xe1, 0xfe, 0xd4, 0x91, 0x29, 0x21, 0x73, 0x5, 0x8e, 0x5e, 0xe1,
        0xcb, 0xdd, 0x6b, 0xda, 0xc0, 0x75, 0x57, 0xc6, 0xa0, 0xe8, 0xd3, 0x68, 0x25, 0xba, 0x15,
        0x9e, 0x7f, 0xb5, 0xad, 0x8c, 0xda, 0xf8, 0x4, 0x86, 0x8c, 0xf9, 0xe, 0x8f, 0x1f, 0x8a,
        0xea, 0x17, 0xc0, 0x16, 0xb5, 0x5c, 0x2a, 0x7a, 0xd4, 0x97, 0xc8, 0x94, 0xfb, 0x71, 0xd7,
        0x53, 0xd7, 0x9b, 0x9a, 0x48, 0x4b, 0x6c, 0x37, 0x6d, 0x72, 0x3b, 0x99, 0x8d, 0x2e, 0x1d,
        0x43, 0x6, 0xbf, 0x10, 0x33, 0xb5, 0xae, 0xf8, 0xcc, 0xa5, 0xcb, 0xb2, 0x56, 0x8b, 0x69,
        0x24, 0x22, 0x6d, 0x22, 0xa3, 0x58, 0xab, 0x7d, 0x87, 0xe4, 0xac, 0x5f, 0x2e, 0x9, 0x1a,
        0xa7, 0x15, 0x79, 0xf3, 0xa5, 0x69, 0x9, 0x49, 0x7d, 0x72, 0xf5, 0x4e, 0x6, 0xba, 0xc1,
        0xc3, 0xb4, 0x41, 0x3b, 0xba, 0x5e, 0xaf, 0x94, 0xc3, 0xb6, 0x4f, 0x34, 0xf9, 0xeb, 0xa4,
        0x1a, 0xcb, 0x6a, 0xe2, 0x83, 0x77, 0x6d, 0x36, 0x46, 0x53, 0x78, 0x48, 0xfe, 0xe8, 0x84,
        0xbd, 0xdd, 0xf5, 0xb1, 0xba, 0x57, 0x98, 0x54, 0xcf, 0xfd, 0xce, 0xba, 0xc3, 0x44, 0x5,
        0x95, 0x27, 0xe5, 0x6d, 0xd5, 0x98, 0xf8, 0xf5, 0x66, 0x71, 0x5a, 0xbe, 0x43, 0x1, 0xdd,
        0x19, 0x11, 0x30, 0xe6, 0xb9, 0xf0, 0xc6, 0x40, 0x39, 0x12, 0x53, 0xe2, 0x29, 0x80, 0x3f,
        0x3a, 0xef, 0x27, 0x4b, 0xed, 0xbf, 0xde, 0x3f, 0xcb, 0xbd, 0x42, 0xea, 0xd6, 0x79,
    ];

    const SAMPLE_AUTH_DATA_MAKE_CREDENTIAL: [u8; 164] = [
        0x58, 0xA2, // bytes(162)
        // authData
        0xc2, 0x89, 0xc5, 0xca, 0x9b, 0x04, 0x60, 0xf9, 0x34, 0x6a, 0xb4, 0xe4, 0x2d, 0x84,
        0x27, // rp_id_hash
        0x43, 0x40, 0x4d, 0x31, 0xf4, 0x84, 0x68, 0x25, 0xa6, 0xd0, 0x65, 0xbe, 0x59, 0x7a,
        0x87, // rp_id_hash
        0x05, 0x1d, // rp_id_hash
        0xC1, // authData Flags
        0x00, 0x00, 0x00, 0x0b, // authData counter
        0xf8, 0xa0, 0x11, 0xf3, 0x8c, 0x0a, 0x4d, 0x15, 0x80, 0x06, 0x17, 0x11, 0x1f, 0x9e, 0xdc,
        0x7d, // AAGUID
        0x00, 0x10, // credential id length
        0x89, 0x59, 0xce, 0xad, 0x5b, 0x5c, 0x48, 0x16, 0x4e, 0x8a, 0xbc, 0xd6, 0xd9, 0x43, 0x5c,
        0x6f, // credential id
        // credential public key
        0xa5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x58, 0x20, 0xa5, 0xfd, 0x5c, 0xe1, 0xb1,
        0xc4, 0x58, 0xc5, 0x30, 0xa5, 0x4f, 0xa6, 0x1b, 0x31, 0xbf, 0x6b, 0x04, 0xbe, 0x8b, 0x97,
        0xaf, 0xde, 0x54, 0xdd, 0x8c, 0xbb, 0x69, 0x27, 0x5a, 0x8a, 0x1b, 0xe1, 0x22, 0x58, 0x20,
        0xfa, 0x3a, 0x32, 0x31, 0xdd, 0x9d, 0xee, 0xd9, 0xd1, 0x89, 0x7b, 0xe5, 0xa6, 0x22, 0x8c,
        0x59, 0x50, 0x1e, 0x4b, 0xcd, 0x12, 0x97, 0x5d, 0x3d, 0xff, 0x73, 0x0f, 0x01, 0x27, 0x8e,
        0xa6, 0x1c, // pub key end
        // Extensions
        0xA1, // map(1)
        0x6B, // text(11)
        0x68, 0x6D, 0x61, 0x63, 0x2D, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, // "hmac-secret"
        0xF5, // true
    ];

    const SAMPLE_AUTH_DATA_GET_ASSERTION: [u8; 229] = [
        0x58, 0xE3, // bytes(227)
        // authData
        0xc2, 0x89, 0xc5, 0xca, 0x9b, 0x04, 0x60, 0xf9, 0x34, 0x6a, 0xb4, 0xe4, 0x2d, 0x84,
        0x27, // rp_id_hash
        0x43, 0x40, 0x4d, 0x31, 0xf4, 0x84, 0x68, 0x25, 0xa6, 0xd0, 0x65, 0xbe, 0x59, 0x7a,
        0x87, // rp_id_hash
        0x05, 0x1d, // rp_id_hash
        0xC1, // authData Flags
        0x00, 0x00, 0x00, 0x0b, // authData counter
        0xf8, 0xa0, 0x11, 0xf3, 0x8c, 0x0a, 0x4d, 0x15, 0x80, 0x06, 0x17, 0x11, 0x1f, 0x9e, 0xdc,
        0x7d, // AAGUID
        0x00, 0x10, // credential id length
        0x89, 0x59, 0xce, 0xad, 0x5b, 0x5c, 0x48, 0x16, 0x4e, 0x8a, 0xbc, 0xd6, 0xd9, 0x43, 0x5c,
        0x6f, // credential id
        // credential public key
        0xa5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x58, 0x20, 0xa5, 0xfd, 0x5c, 0xe1, 0xb1,
        0xc4, 0x58, 0xc5, 0x30, 0xa5, 0x4f, 0xa6, 0x1b, 0x31, 0xbf, 0x6b, 0x04, 0xbe, 0x8b, 0x97,
        0xaf, 0xde, 0x54, 0xdd, 0x8c, 0xbb, 0x69, 0x27, 0x5a, 0x8a, 0x1b, 0xe1, 0x22, 0x58, 0x20,
        0xfa, 0x3a, 0x32, 0x31, 0xdd, 0x9d, 0xee, 0xd9, 0xd1, 0x89, 0x7b, 0xe5, 0xa6, 0x22, 0x8c,
        0x59, 0x50, 0x1e, 0x4b, 0xcd, 0x12, 0x97, 0x5d, 0x3d, 0xff, 0x73, 0x0f, 0x01, 0x27, 0x8e,
        0xa6, 0x1c, // pub key end
        // Extensions
        0xA1, // map(1)
        0x6B, // text(11)
        0x68, 0x6D, 0x61, 0x63, 0x2D, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, // "hmac-secret"
        0x58, 0x40, // bytes(64)
        0x1F, 0x91, 0x52, 0x6C, 0xAE, 0x45, 0x6E, 0x4C, 0xBB, 0x71, 0xC4, 0xDD, 0xE7, 0xBB, 0x87,
        0x71, 0x57, 0xE6, 0xE5, 0x4D, 0xFE, 0xD3, 0x01, 0x5D, 0x7D, 0x4D, 0xBB, 0x22, 0x69, 0xAF,
        0xCD, 0xE6, 0xA9, 0x1B, 0x8D, 0x26, 0x7E, 0xBB, 0xF8, 0x48, 0xEB, 0x95, 0xA6, 0x8E, 0x79,
        0xC7, 0xAC, 0x70, 0x5E, 0x35, 0x1D, 0x54, 0x3D, 0xB0, 0x16, 0x58, 0x87, 0xD6, 0x29, 0x0F,
        0xD4, 0x7A, 0x40, 0xC4,
    ];

    #[test]
    fn parse_cert_chain() {
        let cert: AttestationCertificate = from_slice(&SAMPLE_CERT_CHAIN[1..]).unwrap();
        assert_eq!(&cert.0, &SAMPLE_CERT_CHAIN[4..]);

        let _cert: Vec<AttestationCertificate> = from_slice(&SAMPLE_CERT_CHAIN).unwrap();
    }

    #[test]
    fn parse_attestation_object() {
        let value: AttestationObject = from_slice(&SAMPLE_ATTESTATION).unwrap();
        println!("{value:?}");

        //assert_eq!(true, false);
    }

    #[test]
    fn parse_reader() {
        let v: Vec<u8> = vec![
            0x66, 0x66, 0x6f, 0x6f, 0x62, 0x61, 0x72, 0x66, 0x66, 0x6f, 0x6f, 0x62, 0x61, 0x72,
        ];
        let mut data = Cursor::new(v);
        let value: String = from_slice_stream::<_, _, serde_cbor::Error>(&mut data).unwrap();
        assert_eq!(value, "foobar");
        let mut remaining = Vec::new();
        data.read_to_end(&mut remaining).unwrap();
        assert_eq!(
            remaining.as_slice(),
            &[0x66, 0x66, 0x6f, 0x6f, 0x62, 0x61, 0x72]
        );
        let mut data = Cursor::new(remaining);
        let value: String = from_slice_stream::<_, _, serde_cbor::Error>(&mut data).unwrap();
        assert_eq!(value, "foobar");
        let mut remaining = Vec::new();
        data.read_to_end(&mut remaining).unwrap();
        assert!(remaining.is_empty());
    }

    #[test]
    fn parse_extensions() {
        let auth_make: AuthenticatorData = from_slice(&SAMPLE_AUTH_DATA_MAKE_CREDENTIAL).unwrap();
        assert_eq!(
            auth_make.extensions.hmac_secret,
            Some(HmacSecretResponse::Confirmed(true))
        );
        let auth_get: AuthenticatorData = from_slice(&SAMPLE_AUTH_DATA_GET_ASSERTION).unwrap();
        assert_eq!(
            auth_get.extensions.hmac_secret,
            Some(HmacSecretResponse::Secret(vec![
                0x1F, 0x91, 0x52, 0x6C, 0xAE, 0x45, 0x6E, 0x4C, 0xBB, 0x71, 0xC4, 0xDD, 0xE7, 0xBB,
                0x87, 0x71, 0x57, 0xE6, 0xE5, 0x4D, 0xFE, 0xD3, 0x01, 0x5D, 0x7D, 0x4D, 0xBB, 0x22,
                0x69, 0xAF, 0xCD, 0xE6, 0xA9, 0x1B, 0x8D, 0x26, 0x7E, 0xBB, 0xF8, 0x48, 0xEB, 0x95,
                0xA6, 0x8E, 0x79, 0xC7, 0xAC, 0x70, 0x5E, 0x35, 0x1D, 0x54, 0x3D, 0xB0, 0x16, 0x58,
                0x87, 0xD6, 0x29, 0x0F, 0xD4, 0x7A, 0x40, 0xC4,
            ]))
        );
    }

    /// See: https://github.com/mozilla/authenticator-rs/issues/187
    #[test]
    fn test_aaguid_output() {
        let input = [
            0xcb, 0x69, 0x48, 0x1e, 0x8f, 0xf0, 0x00, 0x39, 0x93, 0xec, 0x0a, 0x27, 0x29, 0xa1,
            0x54, 0xa8,
        ];
        let expected = "AAGuid(cb69481e-8ff0-0039-93ec-0a2729a154a8)";
        let result = AAGuid::from(&input).expect("Failed to parse AAGuid");
        let res_str = format!("{result:?}");
        assert_eq!(expected, &res_str);
    }

    #[test]
    fn test_ad_flags_from_bits() {
        // Check that AuthenticatorDataFlags is defined on the entire u8 range and that
        // `from_bits_truncate` is lossless
        for x in 0..=u8::MAX {
            assert_eq!(
                AuthenticatorDataFlags::from_bits(x),
                Some(AuthenticatorDataFlags::from_bits_truncate(x))
            );
        }
    }
}
