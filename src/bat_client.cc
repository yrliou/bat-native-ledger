/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat_client.h"

#include <algorithm>
#include <sstream>

#include "ledger_impl.h"
#include "bat_helper.h"
#include "rapidjson_bat_helper.h"
#include "static_values.h"

#include "wally_bip39.h"
#include "anon/anon.h"

using namespace std::placeholders;

namespace braveledger_bat_client {

BatClient::BatClient(bat_ledger::LedgerImpl* ledger) :
      ledger_(ledger) {
  initAnonize();
}

BatClient::~BatClient() {
}

void BatClient::registerPersona() {
  auto request_id = ledger_->LoadURL(braveledger_bat_helper::buildURL(REGISTER_PERSONA, PREFIX_V2),
      std::vector<std::string>(), "", "",
      ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::requestCredentialsCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3));
}

void BatClient::requestCredentialsCallback(bool result, const std::string& response, const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  if (!result) {
    ledger_->OnWalletInitialized(ledger::Result::BAD_REGISTRATION_RESPONSE);
    return;
  }

  std::string persona_id = ledger_->GetPersonaId();

  if (persona_id.empty()) {
    persona_id = ledger_->GenerateGUID();
    ledger_->SetPersonaId(persona_id);
  }
  // Anonize2 limit is 31 octets
  std::string user_id = persona_id;
  user_id.erase(std::remove(user_id.begin(), user_id.end(), '-'), user_id.end());
  user_id.erase(12, 1);

  ledger_->SetUserId(user_id);

  std::string registrar_vk = ledger_->GetRegistrarVK();
  if (!braveledger_bat_helper::getJSONValue(REGISTRARVK_FIELDNAME, response, registrar_vk)) {
    ledger_->OnWalletInitialized(ledger::Result::BAD_REGISTRATION_RESPONSE);
    return;
  }
  DCHECK(!registrar_vk.empty());
  ledger_->SetRegistrarVK(registrar_vk);
  std::string pre_flight;
  std::string proof = getAnonizeProof(registrar_vk, user_id, pre_flight);
  ledger_->SetPreFlight(pre_flight);

  if (proof.empty()) {
    ledger_->OnWalletInitialized(ledger::Result::BAD_REGISTRATION_RESPONSE);
    return;
  }

  braveledger_bat_helper::WALLET_INFO_ST wallet_info = ledger_->GetWalletInfo();
  std::vector<uint8_t> key_info_seed = braveledger_bat_helper::generateSeed();

  wallet_info.keyInfoSeed_ = key_info_seed;
  ledger_->SetWalletInfo(wallet_info);
  std::vector<uint8_t> secretKey = braveledger_bat_helper::getHKDF(key_info_seed);
  std::vector<uint8_t> publicKey;
  std::vector<uint8_t> newSecretKey;
  braveledger_bat_helper::getPublicKeyFromSeed(secretKey, publicKey, newSecretKey);
  std::string label = ledger_->GenerateGUID();
  std::string publicKeyHex = braveledger_bat_helper::uint8ToHex(publicKey);
  std::string keys[3] = {"currency", "label", "publicKey"};
  std::string values[3] = {CURRENCY, label, publicKeyHex};
  std::string octets = braveledger_bat_helper::stringify(keys, values, 3);
  std::string headerDigest = "SHA-256=" + braveledger_bat_helper::getBase64(braveledger_bat_helper::getSHA256(octets));
  std::string headerKeys[1] = {"digest"};
  std::string headerValues[1] = {headerDigest};
  std::string headerSignature = braveledger_bat_helper::sign(headerKeys, headerValues, 1, "primary", newSecretKey);

  braveledger_bat_helper::REQUEST_CREDENTIALS_ST requestCredentials;
  requestCredentials.requestType_ = "httpSignature";
  requestCredentials.proof_ = proof;
  requestCredentials.request_body_currency_ = CURRENCY;
  requestCredentials.request_body_label_ = label;
  requestCredentials.request_body_publicKey_ = publicKeyHex;
  requestCredentials.request_headers_digest_ = headerDigest;
  requestCredentials.request_headers_signature_ = headerSignature;
  requestCredentials.request_body_octets_ = octets;
  std::string payloadStringify = braveledger_bat_helper::stringifyRequestCredentialsSt(requestCredentials);
  std::vector<std::string> registerHeaders;
  registerHeaders.push_back("Content-Type: application/json; charset=UTF-8");

  // We should use simple callbacks on iOS
  auto request_id = ledger_->LoadURL(
    braveledger_bat_helper::buildURL((std::string)REGISTER_PERSONA + "/" + ledger_->GetUserId(), PREFIX_V2),
    registerHeaders, payloadStringify, "application/json; charset=utf-8",
    ledger::URL_METHOD::POST, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::registerPersonaCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3));
}

std::string BatClient::getAnonizeProof(const std::string& registrarVK, const std::string& id, std::string& preFlight) {
  const char* cred = makeCred(id.c_str());
  if (nullptr != cred) {
    preFlight = cred;
    free((void*)cred);
  } else {
    return "";
  }
  const char* proofTemp = registerUserMessage(preFlight.c_str(), registrarVK.c_str());
  std::string proof;
  if (nullptr != proofTemp) {
    proof = proofTemp;
    free((void*)proofTemp);
  } else {
    return "";
  }

  return proof;
}

void BatClient::registerPersonaCallback(bool result,
                                       const std::string& response,
                                       const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  if (!result) {
    ledger_->OnWalletInitialized(ledger::Result::BAD_REGISTRATION_RESPONSE);
    return;
  }

  std::string verification;
  if (!braveledger_bat_helper::getJSONValue(VERIFICATION_FIELDNAME, response, verification)) {
    ledger_->OnWalletInitialized(ledger::Result::BAD_REGISTRATION_RESPONSE);
    return;
  }
  const char* masterUserToken = registerUserFinal(ledger_->GetUserId().c_str(), verification.c_str(),
    ledger_->GetPreFlight().c_str(), ledger_->GetRegistrarVK().c_str());
  if (nullptr != masterUserToken) {
    ledger_->SetMasterUserToken(masterUserToken);
    free((void*)masterUserToken);
  } else if (!braveledger_bat_helper::ignore_for_testing()) {
    ledger_->OnWalletInitialized(ledger::Result::REGISTRATION_VERIFICATION_FAILED);
    return;
  }

  braveledger_bat_helper::WALLET_INFO_ST wallet_info = ledger_->GetWalletInfo();
  unsigned int days;
  double fee_amount = .0;
  std::string currency;
  if (!braveledger_bat_helper::getJSONWalletInfo(response, wallet_info, currency, fee_amount, days)) {
    ledger_->OnWalletInitialized(ledger::Result::BAD_REGISTRATION_RESPONSE);
    return;
  }

  ledger_->SetWalletInfo(wallet_info);
  ledger_->SetCurrency(currency);
  ledger_->SetContributionAmount(fee_amount);
  ledger_->SetDays(days);
  ledger_->SetBootStamp(braveledger_bat_helper::currentTime());
  ledger_->ResetReconcileStamp();
  ledger_->OnWalletInitialized(ledger::Result::WALLET_CREATED);
}

void BatClient::getWalletProperties() {
  std::string path = (std::string)WALLET_PROPERTIES + ledger_->GetPaymentId() + WALLET_PROPERTIES_END;
   auto request_id = ledger_->LoadURL(
       braveledger_bat_helper::buildURL(path, PREFIX_V2, braveledger_bat_helper::SERVER_TYPES::BALANCE),
       std::vector<std::string>(),
       "",
       "",
       ledger::URL_METHOD::GET,
       &handler_);

   handler_.AddRequestHandler(std::move(request_id),
                              std::bind(&BatClient::walletPropertiesCallback,
                                        this,
                                        _1,
                                        _2,
                                        _3));
 }

 void BatClient::walletPropertiesCallback(bool success,
                                          const std::string& response,
                                          const std::map<std::string, std::string>& headers) {
   braveledger_bat_helper::WALLET_PROPERTIES_ST properties;
   ledger_->LogResponse(__func__, success, response, headers);
   if (!success) {
     ledger_->OnWalletProperties(ledger::Result::LEDGER_ERROR, properties);
     return;
   }

   bool ok = braveledger_bat_helper::loadFromJson(properties, response);
   if (!ok) {
     ledger_->Log(__func__, ledger::LogLevel::LOG_ERROR, {"Failed to load wallet properties state."});
     ledger_->OnWalletProperties(ledger::Result::LEDGER_ERROR, properties);
     return;
   }
   ledger_->SetWalletProperties(properties);
   ledger_->OnWalletProperties(ledger::Result::LEDGER_OK, properties);
}

void BatClient::prepareBallots() {
  braveledger_bat_helper::Transactions transactions =
      ledger_->GetTransactions();
  braveledger_bat_helper::Ballots ballots = ledger_->GetBallots();
  for (int i = ballots.size() - 1; i >= 0; i--) {
    bool breakTheLoop = false;
    for (size_t j = 0; j < transactions.size(); j++) {
      // TODO check on valid credentials for transaction
      if (transactions[j].viewingId_ == ballots[i].viewingId_
          /*&& (ballots[i].prepareBallot_.empty() || 0 == ballots[i].delayStamp_
            || ballots[i].delayStamp_ <= currentTime)*/) {
        // TODO check on ballot.prepareBallot and call commitBallot if it exist
        if (ballots[i].prepareBallot_.empty()) {
          prepareBatch(ballots[i], transactions[j]);
          breakTheLoop = true;
          break;
        }
      }
    }

    if (breakTheLoop) {
      break;
    }
  }
}

void BatClient::prepareBatch(const braveledger_bat_helper::BALLOT_ST& ballot, const braveledger_bat_helper::TRANSACTION_ST& transaction) {
  auto request_id = ledger_->LoadURL(braveledger_bat_helper::buildURL((std::string)SURVEYOR_BATCH_VOTING + "/" + transaction.anonizeViewingId_, PREFIX_V2),
    std::vector<std::string>(), "", "", ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::prepareBatchCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3));
}

void BatClient::prepareBatchCallback(bool result,
                                     const std::string& response,
                                     const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  std::vector<std::string> surveyors;
  braveledger_bat_helper::getJSONBatchSurveyors(response, surveyors);
  std::vector<braveledger_bat_helper::BATCH_PROOF> batchProof;

  braveledger_bat_helper::Transactions transactions =
    ledger_->GetTransactions();
  braveledger_bat_helper::Ballots ballots = ledger_->GetBallots();

  for (size_t j = 0; j < surveyors.size(); j++) {
    std::string error;
    braveledger_bat_helper::getJSONValue("error", surveyors[j], error);
    if (!error.empty()) {
      continue;
    }

    for (int i = ballots.size() - 1; i >= 0; i--) {
      std::string survId;
      braveledger_bat_helper::getJSONValue("surveyorId", surveyors[j], survId);
      if (ballots[i].surveyorId_ == survId) {
        for (size_t k = 0; k < transactions.size(); k++) {
          if (transactions[k].viewingId_ == ballots[i].viewingId_) {
            ballots[i].prepareBallot_ = surveyors[j];
            braveledger_bat_helper::BATCH_PROOF batchProofEl;
            batchProofEl.transaction_ = transactions[k];
            batchProofEl.ballot_ = ballots[i];
            batchProof.push_back(batchProofEl);
          }
        }
      }
    }
  }

  ledger_->SetBallots(ballots);
  ledger_->RunIOTask(std::bind(&BatClient::proofBatch, this, batchProof, _1));
}

void BatClient::proofBatch(
    const std::vector<braveledger_bat_helper::BATCH_PROOF>& batchProof,
    ledger::LedgerTaskRunner::CallerThreadCallback callback) {
  std::vector<std::string> proofs;

  for (size_t i = 0; i < batchProof.size(); i++) {
    braveledger_bat_helper::SURVEYOR_ST surveyor;
    if (!braveledger_bat_helper::loadFromJson(surveyor, batchProof[i].ballot_.prepareBallot_)) {
      ledger_->Log(__func__, ledger::LogLevel::LOG_ERROR, {"Failed to load surveyor state: ", batchProof[i].ballot_.prepareBallot_});
    }

    std::string signatureToSend;
    size_t delimeterPos = surveyor.signature_.find(',');
    if (std::string::npos != delimeterPos && delimeterPos + 1 <= surveyor.signature_.length()) {
      signatureToSend = surveyor.signature_.substr(delimeterPos + 1);
      if (signatureToSend.length() > 1 && signatureToSend[0] == ' ') {
        signatureToSend.erase(0, 1);
      }
    }

    std::string keysMsg[1] = {"publisher"};
    std::string valuesMsg[1] = {batchProof[i].ballot_.publisher_};
    std::string msg = braveledger_bat_helper::stringify(keysMsg, valuesMsg, 1);

    const char* proof = submitMessage(msg.c_str(), batchProof[i].transaction_.masterUserToken_.c_str(),
      batchProof[i].transaction_.registrarVK_.c_str(), signatureToSend.c_str(), surveyor.surveyorId_.c_str(), surveyor.surveyVK_.c_str());

    std::string anonProof;
    if (nullptr != proof) {
      anonProof = proof;
      free((void*)proof);
    }

    proofs.push_back(anonProof);
  }

  callback(std::bind(&BatClient::proofBatchCallback, this, batchProof, proofs));
}

void BatClient::proofBatchCallback(
    const std::vector<braveledger_bat_helper::BATCH_PROOF>& batchProof,
    const std::vector<std::string>& proofs) {
  braveledger_bat_helper::Ballots ballots = ledger_->GetBallots();
  for (size_t i = 0; i < batchProof.size(); i++) {
    for (size_t j = 0; j < ballots.size(); j++) {
      if (ballots[j].surveyorId_ == batchProof[i].ballot_.surveyorId_) {
        ballots[j].proofBallot_ = proofs[i];
      }
    }
  }
  ledger_->SetBallots(ballots);
  ledger_->PrepareVoteBatchTimer();
}

void BatClient::prepareVoteBatch() {
  braveledger_bat_helper::Transactions transactions =
      ledger_->GetTransactions();
  braveledger_bat_helper::Ballots ballots = ledger_->GetBallots();
    braveledger_bat_helper::BatchVotes batch = ledger_->GetBatch();

  for (int i = ballots.size() - 1; i >= 0; i--) {
    if (ballots[i].prepareBallot_.empty() || ballots[i].proofBallot_.empty()) {
      // TODO error handling
      continue;
    }
    bool transactionExist = false;
    for (size_t k = 0; k < transactions.size(); k++) {
      if (transactions[k].viewingId_ == ballots[i].viewingId_) {
        bool existBallot = false;
        for (size_t j = 0; j < transactions[k].ballots_.size(); j++) {
          if (transactions[k].ballots_[j].publisher_ == ballots[i].publisher_) {
            transactions[k].ballots_[j].offset_++;
            existBallot = true;
            break;
          }
        }
        if (!existBallot) {
          braveledger_bat_helper::TRANSACTION_BALLOT_ST transactionBallot;
          transactionBallot.publisher_ = ballots[i].publisher_;
          transactionBallot.offset_++;
          transactions[k].ballots_.push_back(transactionBallot);
        }
        transactionExist = true;
        break;
      }
    }
    if (!transactionExist) {
      continue;
    }
    bool existBatch = false;
    braveledger_bat_helper::BATCH_VOTES_INFO_ST batchVotesInfoSt;
    batchVotesInfoSt.surveyorId_ = ballots[i].surveyorId_;
    batchVotesInfoSt.proof_ = ballots[i].proofBallot_;

    for (size_t k = 0; k < batch.size(); k++) {
      if (batch[k].publisher_ == ballots[i].publisher_) {
        existBatch = true;
        batch[k].batchVotesInfo_.push_back(batchVotesInfoSt);
      }
    }
    if (!existBatch) {
      braveledger_bat_helper::BATCH_VOTES_ST batchVotesSt;
      batchVotesSt.publisher_ = ballots[i].publisher_;
      batchVotesSt.batchVotesInfo_.push_back(batchVotesInfoSt);
      batch.push_back(batchVotesSt);
    }
    ballots.erase(ballots.begin() + i);
  }

  ledger_->SetTransactions(transactions);
  ledger_->SetBallots(ballots);
  ledger_->SetBatch(batch);
  ledger_->VoteBatchTimer();
}

void BatClient::voteBatch() {
  braveledger_bat_helper::BatchVotes batch = ledger_->GetBatch();
  if (batch.size() == 0) {
    return;
  }

  braveledger_bat_helper::BATCH_VOTES_ST batchVotes = batch[0];
  std::vector<braveledger_bat_helper::BATCH_VOTES_INFO_ST> voteBatch;

  if (batchVotes.batchVotesInfo_.size() > VOTE_BATCH_SIZE) {
    voteBatch.assign(batchVotes.batchVotesInfo_.begin(), batchVotes.batchVotesInfo_.begin() + VOTE_BATCH_SIZE);
  } else {
    voteBatch = batchVotes.batchVotesInfo_;
  }

  std::string payload = braveledger_bat_helper::stringifyBatch(voteBatch);

  auto request_id = ledger_->LoadURL(
    braveledger_bat_helper::buildURL((std::string)SURVEYOR_BATCH_VOTING , PREFIX_V2),
    std::vector<std::string>(), payload, "application/json; charset=utf-8", ledger::URL_METHOD::POST, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::voteBatchCallback,
                                       this,
                                       batchVotes.publisher_,
                                       _1,
                                       _2,
                                       _3));
}

void BatClient::voteBatchCallback(const std::string& publisher,
                                  bool result,
                                  const std::string& response,
                                  const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  std::vector<std::string> surveyors;
  braveledger_bat_helper::getJSONBatchSurveyors(response, surveyors);
  braveledger_bat_helper::BatchVotes batch = ledger_->GetBatch();
  for (size_t i = 0; i < batch.size(); i++) {
    if (batch[i].publisher_ == publisher) {
      size_t sizeToCheck = VOTE_BATCH_SIZE;
      if (batch[i].batchVotesInfo_.size() < VOTE_BATCH_SIZE) {
        sizeToCheck = batch[i].batchVotesInfo_.size();
      }
      for (int j = sizeToCheck - 1; j >= 0; j--) {
        for (size_t k = 0; k < surveyors.size(); k++) {
          std::string surveyorId;
          braveledger_bat_helper::getJSONValue("surveyorId", surveyors[k], surveyorId);
          if (surveyorId == batch[i].batchVotesInfo_[j].surveyorId_) {
            batch[i].batchVotesInfo_.erase(batch[i].batchVotesInfo_.begin() + j);
            break;
          }
        }
      }
      if (0 == batch[i].batchVotesInfo_.size()) {
        batch.erase(batch.begin() + i);
      }
      break;
    }
  }
  ledger_->SetBatch(batch);
  ledger_->VoteBatchTimer();
}

std::string BatClient::getWalletPassphrase() const {
  braveledger_bat_helper::WALLET_INFO_ST wallet_info = ledger_->GetWalletInfo();
  DCHECK(wallet_info.keyInfoSeed_.size());
  std::string passPhrase;
  if (0 == wallet_info.keyInfoSeed_.size()) {
    return passPhrase;
  }
    char* words = nullptr;
  int result = bip39_mnemonic_from_bytes(nullptr, &wallet_info.keyInfoSeed_.front(),
    wallet_info.keyInfoSeed_.size(), &words);
  if (0 != result) {
    DCHECK(false);

    return passPhrase;
  }
  passPhrase = words;
  wally_free_string(words);

  return passPhrase;
}

void BatClient::recoverWallet(const std::string& pass_phrase) {
  size_t written = 0;
  std::vector<unsigned char> newSeed;
  if (braveledger_bat_helper::split(pass_phrase,
    WALLET_PASSPHRASE_DELIM).size() == 16) {
    // use niceware for legacy wallet passphrases
    ledger_->LoadNicewareList(
      std::bind(&BatClient::OnNicewareListLoaded, this, pass_phrase, _1, _2));
  } else {
    std::vector<unsigned char> newSeed;
    newSeed.resize(32);
    int result = bip39_mnemonic_to_bytes
      (nullptr, pass_phrase.c_str(), &newSeed.front(),
      newSeed.size(), &written);
    continueRecover(result, &written, newSeed);
  }
}

void BatClient::OnNicewareListLoaded(const std::string& pass_phrase,
                                        ledger::Result result,
                                        const std::string& data) {
  if (result == ledger::Result::LEDGER_OK &&
    braveledger_bat_helper::split(pass_phrase,
    WALLET_PASSPHRASE_DELIM).size() == 16) {
    std::vector<uint8_t> seed;
    seed.resize(32);
    size_t written = 0;
    uint8_t nwResult = braveledger_bat_helper::niceware_mnemonic_to_bytes(
      pass_phrase, seed, &written, braveledger_bat_helper::split(
      data, DICTIONARY_DELIMITER));
    continueRecover(nwResult, &written, seed);
  } else {
    std::vector<braveledger_bat_helper::GRANT> empty;
    ledger_->OnRecoverWallet(result, 0, empty);
    return;
  }
}

void BatClient::continueRecover(int result, size_t *written, std::vector<uint8_t>& newSeed) {
  if (0 != result || 0 == *written) {
    ledger_->Log(__func__, ledger::LogLevel::LOG_ERROR, {"Result: ", std::to_string(result), " Size: ", std::to_string(*written)});
    std::vector<braveledger_bat_helper::GRANT> empty;
    ledger_->OnRecoverWallet(ledger::Result::LEDGER_ERROR, 0, empty);
    return;
  }


  braveledger_bat_helper::WALLET_INFO_ST wallet_info = ledger_->GetWalletInfo();
  wallet_info.keyInfoSeed_ = newSeed;
  ledger_->SetWalletInfo(wallet_info);

  std::vector<uint8_t> secretKey = braveledger_bat_helper::getHKDF(newSeed);
  std::vector<uint8_t> publicKey;
  std::vector<uint8_t> newSecretKey;
  braveledger_bat_helper::getPublicKeyFromSeed(secretKey, publicKey, newSecretKey);
  std::string publicKeyHex = braveledger_bat_helper::uint8ToHex(publicKey);

  auto request_id = ledger_->LoadURL(braveledger_bat_helper::buildURL((std::string)RECOVER_WALLET_PUBLIC_KEY + publicKeyHex, PREFIX_V2),
    std::vector<std::string>(), "", "",
    ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::recoverWalletPublicKeyCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3));

}

void BatClient::recoverWalletPublicKeyCallback(bool result,
                                               const std::string& response,
                                               const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  if (!result) {
    std::vector<braveledger_bat_helper::GRANT> empty;
    ledger_->OnRecoverWallet(ledger::Result::LEDGER_ERROR, 0, empty);
    return;
  }
  std::string recoveryId;
  braveledger_bat_helper::getJSONValue("paymentId", response, recoveryId);

  auto request_id = ledger_->LoadURL(braveledger_bat_helper::buildURL((std::string)WALLET_PROPERTIES + recoveryId, PREFIX_V2),
    std::vector<std::string>(), "", "", ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::recoverWalletCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3,
                                       recoveryId));
}

void BatClient::recoverWalletCallback(bool result,
                                      const std::string& response,
                                      const std::map<std::string, std::string>& headers,
                                      const std::string& recoveryId) {
  ledger_->LogResponse(__func__, result, response, headers);
  if (!result) {
    std::vector<braveledger_bat_helper::GRANT> empty;
    ledger_->OnRecoverWallet(ledger::Result::LEDGER_ERROR, 0, empty);
    return;
  }

  braveledger_bat_helper::WALLET_INFO_ST wallet_info = ledger_->GetWalletInfo();
  braveledger_bat_helper::WALLET_PROPERTIES_ST properties =
      ledger_->GetWalletProperties();
  unsigned int days;
  double fee_amount = .0;
  std::string currency;
  braveledger_bat_helper::getJSONWalletInfo(response, wallet_info, currency, fee_amount, days);
  braveledger_bat_helper::getJSONRecoverWallet(response, properties.balance_, properties.probi_, properties.grants_);
  ledger_->SetWalletInfo(wallet_info);
  ledger_->SetCurrency(currency);
  ledger_->SetContributionAmount(fee_amount);
  ledger_->SetDays(days);
  ledger_->SetWalletProperties(properties);
  ledger_->SetPaymentId(recoveryId);
  ledger_->OnRecoverWallet(ledger::Result::LEDGER_OK, properties.balance_, properties.grants_);
}

void BatClient::getGrant(const std::string& lang, const std::string& forPaymentId) {
  std::string paymentId = forPaymentId;
  if (paymentId.empty()) {
    paymentId = ledger_->GetPaymentId();
  }
  std::string arguments;
  if (!paymentId.empty() || !lang.empty()) {
    arguments = "?";
    if (!paymentId.empty()) {
      arguments += "paymentId=" + paymentId;
    }
    if (!lang.empty()) {
      if (arguments.length() > 1) {
        arguments += "&";
      }
      arguments += "lang=" + lang;
    }
  }

  auto request_id = ledger_->LoadURL(braveledger_bat_helper::buildURL((std::string)GET_SET_PROMOTION + arguments, PREFIX_V2),
    std::vector<std::string>(), "", "", ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::getGrantCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3));
}

void BatClient::getGrantCallback(bool success,
                                 const std::string& response,
                                 const std::map<std::string, std::string>& headers) {
  braveledger_bat_helper::GRANT properties;

  ledger_->LogResponse(__func__, success, response, headers);

  if (!success) {
    ledger_->OnGrant(ledger::Result::LEDGER_ERROR, properties);
    return;
  }

  bool ok = braveledger_bat_helper::loadFromJson(properties, response);

  if (!ok) {
    ledger_->OnGrant(ledger::Result::LEDGER_ERROR, properties);
    return;
  }

  ledger_->SetLastGrantLoadTimestamp(time(0));
  ledger_->SetGrant(properties);
  ledger_->OnGrant(ledger::Result::LEDGER_OK, properties);
}

void BatClient::setGrant(const std::string& captchaResponse, const std::string& promotionId) {
  braveledger_bat_helper::GRANT state_grant = ledger_->GetGrant();
  std::string state_promotionId = state_grant.promotionId;

  if (promotionId.empty() && state_promotionId.empty()) {
    braveledger_bat_helper::GRANT properties;
    ledger_->OnGrantFinish(ledger::Result::LEDGER_ERROR, properties);
    return;
  }

  std::string promoId = state_promotionId;
  if (!promotionId.empty()) {
    promoId = promotionId;
  }

  std::string keys[2] = {"promotionId", "captchaResponse"};
  std::string values[2] = {promoId, captchaResponse};
  std::string payload = braveledger_bat_helper::stringify(keys, values, 2);

  auto request_id = ledger_->LoadURL(braveledger_bat_helper::buildURL((std::string)GET_SET_PROMOTION + "/" + ledger_->GetPaymentId(), PREFIX_V2),
       std::vector<std::string>(), payload, "application/json; charset=utf-8", ledger::URL_METHOD::PUT, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::setGrantCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3));
}

void BatClient::setGrantCallback(bool success,
                                 const std::string& response,
                                 const std::map<std::string, std::string>& headers) {
  std::string error;
  unsigned int statusCode;
  braveledger_bat_helper::GRANT grant;
  braveledger_bat_helper::getJSONResponse(response, statusCode, error);

  ledger_->LogResponse(__func__, success, response, headers);

  if (!success) {
    if (statusCode == 403) {
      ledger_->OnGrantFinish(ledger::Result::CAPTCHA_FAILED, grant);
    } else if (statusCode == 404 || statusCode == 410) {
      ledger_->OnGrantFinish(ledger::Result::GRANT_NOT_FOUND, grant);
    } else {
      ledger_->OnGrantFinish(ledger::Result::LEDGER_ERROR, grant);
    }
    return;
  }

  bool ok = braveledger_bat_helper::loadFromJson(grant, response);
  if (!ok) {
    ledger_->OnGrantFinish(ledger::Result::LEDGER_ERROR, grant);
    return;
  }

  braveledger_bat_helper::GRANT state_grant = ledger_->GetGrant();
  grant.promotionId = state_grant.promotionId;
  ledger_->SetGrant(grant);

  ledger_->OnGrantFinish(ledger::Result::LEDGER_OK, grant);
}

void BatClient::getGrantCaptcha() {
  std::vector<std::string> headers;
  headers.push_back("brave-product:brave-core");
  auto request_id = ledger_->LoadURL(braveledger_bat_helper::buildURL((std::string)GET_PROMOTION_CAPTCHA + ledger_->GetPaymentId(), PREFIX_V2),
   headers, "", "",
      ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatClient::getGrantCaptchaCallback,
                                       this,
                                       _1,
                                       _2,
                                       _3));
}

void BatClient::getGrantCaptchaCallback(bool success,
                                        const std::string& response,
                                        const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, success, response, headers);

  auto it = headers.find("captcha-hint");
  if (!success || it == headers.end()) {
    // TODO NZ Add error handler
    return;
  }

  ledger_->OnGrantCaptcha(response, it->second);
}

}  // namespace braveledger_bat_client
