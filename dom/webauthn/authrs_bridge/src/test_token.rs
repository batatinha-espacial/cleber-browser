/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use authenticator::authenticatorservice::{RegisterArgs, SignArgs};
use authenticator::crypto::{ecdsa_p256_sha256_sign_raw, COSEAlgorithm, COSEKey, SharedSecret};
use authenticator::ctap2::{
    attestation::{
        AAGuid, AttestationObject, AttestationStatement, AttestationStatementPacked,
        AttestedCredentialData, AuthenticatorData, AuthenticatorDataFlags, Extension,
    },
    client_data::ClientDataHash,
    commands::{
        client_pin::{ClientPIN, ClientPinResponse, PINSubcommand},
        get_assertion::{Assertion, GetAssertion, GetAssertionResponse, GetAssertionResult},
        get_info::{AuthenticatorInfo, AuthenticatorOptions, AuthenticatorVersion},
        get_version::{GetVersion, U2FInfo},
        make_credentials::{MakeCredentials, MakeCredentialsResult},
        reset::Reset,
        selection::Selection,
        Request, RequestCtap1, RequestCtap2, StatusCode,
    },
    preflight::CheckKeyHandle,
    server::{PublicKeyCredentialDescriptor, RelyingParty, RelyingPartyWrapper, User},
};
use authenticator::errors::{AuthenticatorError, CommandError, HIDError, U2FTokenError};
use authenticator::{ctap2, statecallback::StateCallback};
use authenticator::{FidoDevice, FidoDeviceIO, FidoProtocol, VirtualFidoDevice};
use authenticator::{RegisterResult, SignResult, StatusUpdate};
use nserror::{nsresult, NS_ERROR_FAILURE, NS_ERROR_INVALID_ARG, NS_ERROR_NOT_IMPLEMENTED, NS_OK};
use nsstring::{nsACString, nsCString};
use rand::{thread_rng, RngCore};
use std::cell::{Ref, RefCell};
use std::collections::{hash_map::Entry, HashMap};
use std::ops::{Deref, DerefMut};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::Sender;
use std::sync::Mutex;
use thin_vec::ThinVec;
use xpcom::interfaces::nsICredentialParameters;
use xpcom::{xpcom_method, RefPtr};

// All TestTokens use this fixed, randomly generated, AAGUID
const VIRTUAL_TOKEN_AAGUID: AAGuid = AAGuid([
    0x68, 0xe1, 0x00, 0xa5, 0x0b, 0x47, 0x91, 0x04, 0xb8, 0x54, 0x97, 0xa9, 0xba, 0x51, 0x06, 0x38,
]);

#[derive(Debug)]
struct TestTokenCredential {
    id: Vec<u8>,
    privkey: Vec<u8>,
    user_handle: Vec<u8>,
    sign_count: AtomicU32,
    is_discoverable_credential: bool,
    rp: RelyingPartyWrapper,
}

impl TestTokenCredential {
    fn assert(
        &self,
        client_data_hash: &ClientDataHash,
        flags: AuthenticatorDataFlags,
    ) -> GetAssertionResponse {
        let credentials = Some(PublicKeyCredentialDescriptor {
            id: self.id.clone(),
            transports: vec![],
        });

        let auth_data = AuthenticatorData {
            rp_id_hash: self.rp.hash(),
            flags,
            counter: self.sign_count.fetch_add(1, Ordering::Relaxed),
            credential_data: None,
            extensions: Extension::default(),
        };

        let user = Some(User {
            id: self.user_handle.clone(),
            ..Default::default()
        });

        let mut data = auth_data.to_vec().unwrap();
        data.extend_from_slice(client_data_hash.as_ref());
        let signature = ecdsa_p256_sha256_sign_raw(&self.privkey, &data).unwrap();
        GetAssertionResponse {
            credentials,
            auth_data,
            signature,
            user,
            number_of_credentials: Some(1),
        }
    }
}

#[derive(Debug)]
struct TestToken {
    protocol: FidoProtocol,
    versions: Vec<AuthenticatorVersion>,
    has_resident_key: bool,
    has_user_verification: bool,
    is_user_consenting: bool,
    is_user_verified: bool,
    // This is modified in `make_credentials` which takes a &TestToken, but we only allow one transaction at a time.
    credentials: RefCell<Vec<TestTokenCredential>>,
    pin_token: [u8; 32],
    shared_secret: Option<SharedSecret>,
    authenticator_info: Option<AuthenticatorInfo>,
}

impl TestToken {
    fn new(
        versions: Vec<AuthenticatorVersion>,
        has_resident_key: bool,
        has_user_verification: bool,
        is_user_consenting: bool,
        is_user_verified: bool,
    ) -> TestToken {
        let mut pin_token = [0u8; 32];
        thread_rng().fill_bytes(&mut pin_token);
        Self {
            protocol: FidoProtocol::CTAP2,
            versions,
            has_resident_key,
            has_user_verification,
            is_user_consenting,
            is_user_verified,
            credentials: RefCell::new(vec![]),
            pin_token,
            shared_secret: None,
            authenticator_info: None,
        }
    }

    fn insert_credential(
        &self,
        id: &[u8],
        privkey: &[u8],
        rp_id: &RelyingPartyWrapper,
        is_discoverable_credential: bool,
        user_handle: &[u8],
        sign_count: u32,
    ) {
        let c = TestTokenCredential {
            id: id.to_vec(),
            privkey: privkey.to_vec(),
            rp: rp_id.clone(),
            is_discoverable_credential,
            user_handle: user_handle.to_vec(),
            sign_count: AtomicU32::new(sign_count),
        };

        let mut credlist = self.credentials.borrow_mut();

        match credlist.binary_search_by_key(&id, |probe| &probe.id) {
            Ok(_) => {}
            Err(idx) => credlist.insert(idx, c),
        }
    }

    fn get_credentials(&self) -> Ref<Vec<TestTokenCredential>> {
        self.credentials.borrow()
    }

    fn delete_credential(&mut self, id: &[u8]) -> bool {
        let mut credlist = self.credentials.borrow_mut();
        if let Ok(idx) = credlist.binary_search_by_key(&id, |probe| &probe.id) {
            credlist.remove(idx);
            return true;
        }

        false
    }

    fn delete_all_credentials(&mut self) {
        self.credentials.borrow_mut().clear();
    }

    fn has_credential(&self, id: &[u8]) -> bool {
        self.credentials
            .borrow()
            .binary_search_by_key(&id, |probe| &probe.id)
            .is_ok()
    }
}

impl FidoDevice for TestToken {
    fn pre_init(&mut self) -> Result<(), HIDError> {
        Ok(())
    }

    fn should_try_ctap2(&self) -> bool {
        true
    }

    fn initialized(&self) -> bool {
        true
    }

    fn is_u2f(&mut self) -> bool {
        true
    }

    fn get_shared_secret(&self) -> Option<&SharedSecret> {
        self.shared_secret.as_ref()
    }

    fn set_shared_secret(&mut self, shared_secret: SharedSecret) {
        self.shared_secret = Some(shared_secret);
    }

    fn get_authenticator_info(&self) -> Option<&AuthenticatorInfo> {
        self.authenticator_info.as_ref()
    }

    fn set_authenticator_info(&mut self, authenticator_info: AuthenticatorInfo) {
        self.authenticator_info = Some(authenticator_info);
    }

    fn get_protocol(&self) -> FidoProtocol {
        self.protocol
    }

    fn downgrade_to_ctap1(&mut self) {
        self.protocol = FidoProtocol::CTAP1
    }
}

impl FidoDeviceIO for TestToken {
    fn send_msg_cancellable<Out, Req: Request<Out>>(
        &mut self,
        msg: &Req,
        keep_alive: &dyn Fn() -> bool,
    ) -> Result<Out, HIDError> {
        if !self.initialized() {
            return Err(HIDError::DeviceNotInitialized);
        }

        match self.get_protocol() {
            FidoProtocol::CTAP1 => self.send_ctap1_cancellable(msg, keep_alive),
            FidoProtocol::CTAP2 => self.send_cbor_cancellable(msg, keep_alive),
        }
    }

    fn send_cbor_cancellable<Req: RequestCtap2>(
        &mut self,
        msg: &Req,
        _keep_alive: &dyn Fn() -> bool,
    ) -> Result<Req::Output, HIDError> {
        msg.send_to_virtual_device(self)
    }

    fn send_ctap1_cancellable<Req: RequestCtap1>(
        &mut self,
        msg: &Req,
        _keep_alive: &dyn Fn() -> bool,
    ) -> Result<Req::Output, HIDError> {
        msg.send_to_virtual_device(self)
    }
}

impl VirtualFidoDevice for TestToken {
    fn check_key_handle(&self, _req: &CheckKeyHandle) -> Result<(), HIDError> {
        Err(HIDError::UnsupportedCommand)
    }

    fn client_pin(&self, req: &ClientPIN) -> Result<ClientPinResponse, HIDError> {
        match req.subcommand {
            PINSubcommand::GetKeyAgreement => {
                // We don't need to save, or even know, the private key for the public key returned
                // here because we have access to the shared secret derived on the client side.
                let (_private, public) = COSEKey::generate(COSEAlgorithm::ECDH_ES_HKDF256)
                    .map_err(|_| HIDError::DeviceError)?;
                Ok(ClientPinResponse {
                    key_agreement: Some(public),
                    ..Default::default()
                })
            }
            PINSubcommand::GetPinUvAuthTokenUsingUvWithPermissions => {
                // TODO: permissions
                if !self.is_user_consenting || !self.is_user_verified {
                    return Err(HIDError::Command(CommandError::StatusCode(
                        StatusCode::OperationDenied,
                        None,
                    )));
                }
                let secret = match self.shared_secret.as_ref() {
                    Some(secret) => secret,
                    _ => return Err(HIDError::DeviceError),
                };
                let encrypted_pin_token = match secret.encrypt(&self.pin_token) {
                    Ok(token) => token,
                    _ => return Err(HIDError::DeviceError),
                };
                Ok(ClientPinResponse {
                    pin_token: Some(encrypted_pin_token),
                    ..Default::default()
                })
            }
            _ => Err(HIDError::UnsupportedCommand),
        }
    }

    fn get_assertion(&self, req: &GetAssertion) -> Result<GetAssertionResult, HIDError> {
        // Algorithm 6.2.2 from CTAP 2.1
        // https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#sctn-makeCred-authnr-alg

        // 1. zero length pinUvAuthParam
        // (not implemented)

        // 2. Validate pinUvAuthParam
        // Handled by caller

        // 3. Initialize "uv" and "up" bits to false
        let mut flags = AuthenticatorDataFlags::empty();

        // 4. Handle all options
        // 4.1 and 4.2
        let effective_uv_opt =
            req.options.user_verification.unwrap_or(false) && req.pin_uv_auth_param.is_none();

        // 4.3
        if effective_uv_opt && !self.has_user_verification {
            return Err(HIDError::Command(CommandError::StatusCode(
                StatusCode::InvalidOption,
                None,
            )));
        }

        // 4.4 rk
        // (not implemented, we don't encode it)

        // 4.5
        let effective_up_opt = req.options.user_presence.unwrap_or(true);

        // 5. alwaysUv
        // (not implemented)

        // 6. User verification
        // TODO: Permissions, (maybe) validate pinUvAuthParam
        if self.is_user_verified && (effective_uv_opt || req.pin_uv_auth_param.is_some()) {
            flags |= AuthenticatorDataFlags::USER_VERIFIED;
        }

        // 7. Locate credentials
        let credlist = self.credentials.borrow();
        let req_rp_hash = req.rp.hash();
        let eligible_cred_iter = credlist.iter().filter(|x| x.rp.hash() == req_rp_hash);

        // 8. Set up=true if evidence of user interaction was provided in step 6.
        // (not applicable, we use pinUvAuthParam)

        // 9. User presence test
        if self.is_user_consenting && effective_up_opt {
            flags |= AuthenticatorDataFlags::USER_PRESENT;
        }

        // 10. Extensions
        // (not implemented)

        let mut assertions: Vec<Assertion> = vec![];
        if !req.allow_list.is_empty() {
            // 11. Non-discoverable credential case
            // return at most one assertion matching an allowed credential ID
            for credential in eligible_cred_iter {
                if req.allow_list.iter().any(|x| x.id == credential.id) {
                    let assertion = credential.assert(&req.client_data_hash, flags).into();
                    assertions.push(assertion);
                    break;
                }
            }
        } else {
            // 12. Discoverable credential case
            // return any number of assertions from credentials bound to this RP ID
            // TODO(Bug 1838932) Until we have conditional mediation we actually don't want to
            // return a list of credentials here. The UI to select one of the results blocks
            // testing.
            for credential in eligible_cred_iter.filter(|x| x.is_discoverable_credential) {
                let assertion = credential.assert(&req.client_data_hash, flags).into();
                assertions.push(assertion);
                break;
            }
        }

        Ok(GetAssertionResult(assertions))
    }

    fn get_info(&self) -> Result<AuthenticatorInfo, HIDError> {
        // This is a CTAP2.1 device with internal user verification support
        Ok(AuthenticatorInfo {
            versions: self.versions.clone(),
            options: AuthenticatorOptions {
                pin_uv_auth_token: Some(true),
                user_verification: Some(true),
                ..Default::default()
            },
            ..Default::default()
        })
    }

    fn get_version(&self, _req: &GetVersion) -> Result<U2FInfo, HIDError> {
        Err(HIDError::UnsupportedCommand)
    }

    fn make_credentials(&self, req: &MakeCredentials) -> Result<MakeCredentialsResult, HIDError> {
        // Algorithm 6.1.2 from CTAP 2.1
        // https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#sctn-makeCred-authnr-alg

        // 1. zero length pinUvAuthParam
        // (not implemented)

        // 2. Validate pinUvAuthParam
        // Handled by caller

        // 3. Validate pubKeyCredParams
        if !req
            .pub_cred_params
            .iter()
            .any(|x| x.alg == COSEAlgorithm::ES256)
        {
            return Err(HIDError::Command(CommandError::StatusCode(
                StatusCode::UnsupportedAlgorithm,
                None,
            )));
        }

        // 4. initialize "uv" and "up" bits to false
        let mut flags = AuthenticatorDataFlags::empty();

        // 5. process all options

        // 5.1 and 5.2
        let effective_uv_opt =
            req.options.user_verification.unwrap_or(false) && req.pin_uv_auth_param.is_none();

        // 5.3
        if effective_uv_opt && !self.has_user_verification {
            return Err(HIDError::Command(CommandError::StatusCode(
                StatusCode::InvalidOption,
                None,
            )));
        }

        // 5.4
        if req.options.resident_key.unwrap_or(false) && !self.has_resident_key {
            return Err(HIDError::Command(CommandError::StatusCode(
                StatusCode::UnsupportedOption,
                None,
            )));
        }

        // 5.6 and 5.7
        // Nothing to do. We don't provide a way to set up=false.

        // 6. alwaysUv option ID
        // (not implemented)

        // 7. and 8. makeCredUvNotRqd option ID
        // (not implemented)

        // 9. enterprise attestation
        // (not implemented)

        // 11. User verification
        // TODO: Permissions, (maybe) validate pinUvAuthParam
        if self.is_user_verified {
            flags |= AuthenticatorDataFlags::USER_VERIFIED;
        }

        // 12. exclude list
        // TODO: credProtect
        if req.exclude_list.iter().any(|x| self.has_credential(&x.id)) {
            return Err(HIDError::Command(CommandError::StatusCode(
                StatusCode::CredentialExcluded,
                None,
            )));
        }

        // 13. Set up=true if evidence of user interaction was provided in step 11.
        // (not applicable, we use pinUvAuthParam)

        // 14. User presence test
        if self.is_user_consenting {
            flags |= AuthenticatorDataFlags::USER_PRESENT;
        }

        // 15. process extensions
        // (not implemented)

        // 16. Generate a new credential.
        let (private, public) =
            COSEKey::generate(COSEAlgorithm::ES256).map_err(|_| HIDError::DeviceError)?;
        let counter = 0;

        // 17. and 18. Store credential
        //
        // All of the credentials that we create are "resident"---we store the private key locally,
        // and use a random value for the credential ID. The `req.options.resident_key` field
        // determines whether we make the credential "discoverable".
        let mut id = [0u8; 32];
        thread_rng().fill_bytes(&mut id);
        self.insert_credential(
            &id,
            &private,
            &req.rp,
            req.options.resident_key.unwrap_or(false),
            &req.user.clone().unwrap_or_default().id,
            counter,
        );

        // 19. Generate attestation statement
        flags |= AuthenticatorDataFlags::ATTESTED;

        let auth_data = AuthenticatorData {
            rp_id_hash: req.rp.hash(),
            flags,
            counter,
            credential_data: Some(AttestedCredentialData {
                aaguid: VIRTUAL_TOKEN_AAGUID,
                credential_id: id.to_vec(),
                credential_public_key: public,
            }),
            extensions: Extension::default(),
        };

        let mut data = auth_data.to_vec().unwrap();
        data.extend_from_slice(req.client_data_hash.as_ref());
        let sig = ecdsa_p256_sha256_sign_raw(&private, &data).unwrap();

        let att_statement = AttestationStatement::Packed(AttestationStatementPacked {
            alg: COSEAlgorithm::ES256,
            sig: sig.as_slice().into(),
            attestation_cert: vec![],
        });

        let result = MakeCredentialsResult(AttestationObject {
            auth_data,
            att_statement,
        });
        Ok(result)
    }

    fn reset(&self, _req: &Reset) -> Result<(), HIDError> {
        Err(HIDError::UnsupportedCommand)
    }

    fn selection(&self, _req: &Selection) -> Result<(), HIDError> {
        Err(HIDError::UnsupportedCommand)
    }
}

#[xpcom(implement(nsICredentialParameters), atomic)]
struct CredentialParameters {
    credential_id: Vec<u8>,
    is_resident_credential: bool,
    rp_id: String,
    private_key: Vec<u8>,
    user_handle: Vec<u8>,
    sign_count: u32,
}

impl CredentialParameters {
    xpcom_method!(get_credential_id => GetCredentialId() -> nsACString);
    fn get_credential_id(&self) -> Result<nsCString, nsresult> {
        Ok(nsCString::from(&self.credential_id))
    }

    xpcom_method!(get_is_resident_credential => GetIsResidentCredential() -> bool);
    fn get_is_resident_credential(&self) -> Result<bool, nsresult> {
        Ok(self.is_resident_credential)
    }

    xpcom_method!(get_rp_id => GetRpId() -> nsACString);
    fn get_rp_id(&self) -> Result<nsCString, nsresult> {
        Ok(nsCString::from(&self.rp_id))
    }

    xpcom_method!(get_private_key => GetPrivateKey() -> nsACString);
    fn get_private_key(&self) -> Result<nsCString, nsresult> {
        Ok(nsCString::from(&self.private_key))
    }

    xpcom_method!(get_user_handle => GetUserHandle() -> nsACString);
    fn get_user_handle(&self) -> Result<nsCString, nsresult> {
        Ok(nsCString::from(&self.user_handle))
    }

    xpcom_method!(get_sign_count => GetSignCount() -> u32);
    fn get_sign_count(&self) -> Result<u32, nsresult> {
        Ok(self.sign_count)
    }
}

#[derive(Default)]
pub(crate) struct TestTokenManager {
    state: Mutex<HashMap<u64, TestToken>>,
}

impl TestTokenManager {
    pub fn new() -> Self {
        Default::default()
    }

    pub fn add_virtual_authenticator(
        &self,
        protocol: AuthenticatorVersion,
        has_resident_key: bool,
        has_user_verification: bool,
        is_user_consenting: bool,
        is_user_verified: bool,
    ) -> Result<u64, nsresult> {
        let mut guard = self.state.lock().map_err(|_| NS_ERROR_FAILURE)?;
        let token = TestToken::new(
            vec![protocol],
            has_resident_key,
            has_user_verification,
            is_user_consenting,
            is_user_verified,
        );
        loop {
            let id = rand::random::<u64>() & 0x1f_ffff_ffff_ffffu64; // Make the id safe for JS (53 bits)
            match guard.deref_mut().entry(id) {
                Entry::Occupied(_) => continue,
                Entry::Vacant(v) => {
                    v.insert(token);
                    return Ok(id);
                }
            };
        }
    }

    pub fn remove_virtual_authenticator(&self, authenticator_id: u64) -> Result<(), nsresult> {
        let mut guard = self.state.lock().map_err(|_| NS_ERROR_FAILURE)?;
        guard
            .deref_mut()
            .remove(&authenticator_id)
            .ok_or(NS_ERROR_INVALID_ARG)?;
        Ok(())
    }

    pub fn add_credential(
        &self,
        authenticator_id: u64,
        id: &[u8],
        privkey: &[u8],
        user_handle: &[u8],
        sign_count: u32,
        rp_id: String,
        is_resident_credential: bool,
    ) -> Result<(), nsresult> {
        let mut guard = self.state.lock().map_err(|_| NS_ERROR_FAILURE)?;
        let token = guard
            .deref_mut()
            .get_mut(&authenticator_id)
            .ok_or(NS_ERROR_INVALID_ARG)?;
        let rp = RelyingParty {
            id: rp_id,
            name: None,
            icon: None,
        };
        token.insert_credential(
            id,
            privkey,
            &RelyingPartyWrapper::Data(rp),
            is_resident_credential,
            user_handle,
            sign_count,
        );
        Ok(())
    }

    pub fn get_credentials(
        &self,
        authenticator_id: u64,
    ) -> Result<ThinVec<Option<RefPtr<nsICredentialParameters>>>, nsresult> {
        let mut guard = self.state.lock().map_err(|_| NS_ERROR_FAILURE)?;
        let token = guard
            .get_mut(&authenticator_id)
            .ok_or(NS_ERROR_INVALID_ARG)?;
        let credentials = token.get_credentials();
        let mut credentials_parameters = ThinVec::with_capacity(credentials.len());
        for credential in credentials.deref() {
            // CTAP1 credentials are not currently supported here.
            let rp_id = credential
                .rp
                .id()
                .map(|id| id.clone())
                .ok_or(NS_ERROR_NOT_IMPLEMENTED)?;
            let credential_parameters = CredentialParameters::allocate(InitCredentialParameters {
                credential_id: credential.id.clone(),
                is_resident_credential: credential.is_discoverable_credential,
                rp_id,
                private_key: credential.privkey.clone(),
                user_handle: credential.user_handle.clone(),
                sign_count: credential.sign_count.load(Ordering::Relaxed),
            })
            .query_interface::<nsICredentialParameters>()
            .ok_or(NS_ERROR_FAILURE)?;
            credentials_parameters.push(Some(credential_parameters));
        }
        Ok(credentials_parameters)
    }

    pub fn remove_credential(&self, authenticator_id: u64, id: &[u8]) -> Result<(), nsresult> {
        let mut guard = self.state.lock().map_err(|_| NS_ERROR_FAILURE)?;
        let token = guard
            .deref_mut()
            .get_mut(&authenticator_id)
            .ok_or(NS_ERROR_INVALID_ARG)?;
        if token.delete_credential(id) {
            Ok(())
        } else {
            Err(NS_ERROR_INVALID_ARG)
        }
    }

    pub fn remove_all_credentials(&self, authenticator_id: u64) -> Result<(), nsresult> {
        let mut guard = self.state.lock().map_err(|_| NS_ERROR_FAILURE)?;
        let token = guard
            .deref_mut()
            .get_mut(&authenticator_id)
            .ok_or(NS_ERROR_INVALID_ARG)?;
        token.delete_all_credentials();
        Ok(())
    }

    pub fn set_user_verified(
        &self,
        authenticator_id: u64,
        is_user_verified: bool,
    ) -> Result<(), nsresult> {
        let mut guard = self.state.lock().map_err(|_| NS_ERROR_FAILURE)?;
        let token = guard
            .deref_mut()
            .get_mut(&authenticator_id)
            .ok_or(NS_ERROR_INVALID_ARG)?;
        token.is_user_verified = is_user_verified;
        Ok(())
    }

    pub fn register(
        &self,
        _timeout: u64,
        ctap_args: RegisterArgs,
        status: Sender<StatusUpdate>,
        callback: StateCallback<Result<RegisterResult, AuthenticatorError>>,
    ) {
        if !static_prefs::pref!("security.webauth.webauthn_enable_softtoken") {
            return;
        }

        let mut state_obj = self.state.lock().unwrap();

        // We query the tokens sequentially since the register operation will not block.
        for token in state_obj.values_mut() {
            let _ = token.init();
            if ctap2::register(
                token,
                ctap_args.clone(),
                status.clone(),
                callback.clone(),
                &|| true,
            ) {
                // callback was called
                return;
            }
        }

        // Send an error, if the callback wasn't called already.
        callback.call(Err(AuthenticatorError::U2FToken(U2FTokenError::NotAllowed)));
    }

    pub fn sign(
        &self,
        _timeout: u64,
        ctap_args: SignArgs,
        status: Sender<StatusUpdate>,
        callback: StateCallback<Result<SignResult, AuthenticatorError>>,
    ) {
        if !static_prefs::pref!("security.webauth.webauthn_enable_softtoken") {
            return;
        }

        let mut state_obj = self.state.lock().unwrap();

        // We query the tokens sequentially since the sign operation will not block.
        for token in state_obj.values_mut() {
            let _ = token.init();
            if ctap2::sign(
                token,
                ctap_args.clone(),
                status.clone(),
                callback.clone(),
                &|| true,
            ) {
                // callback was called
                return;
            }
        }
        // Send an error, if the callback wasn't called already.
        callback.call(Err(AuthenticatorError::U2FToken(U2FTokenError::NotAllowed)));
    }
}
