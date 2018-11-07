/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <vector>
#include <map>

#include "bat_contribution.h"
#include "ledger_impl.h"
#include "anon/anon.h"

namespace braveledger_bat_contribution {

BatContribution::BatContribution(bat_ledger::LedgerImpl* ledger) :
    ledger_(ledger) {
  initAnonize();
}

BatContribution::~BatContribution() {
}

// TODO(nejczdovc) we have the same function in bat-client
// Maybe create anonize helper?
std::string BatContribution::GetAnonizeProof(
    const std::string& registrar_VK,
    const std::string& id,
    std::string& pre_flight) {
  const char* cred = makeCred(id.c_str());
  if (nullptr != cred) {
    pre_flight = cred;
    free((void*)cred);
  } else {
    return "";
  }
  const char* proofTemp = registerUserMessage(pre_flight.c_str(),
                                              registrar_VK.c_str());
  std::string proof;
  if (nullptr != proofTemp) {
    proof = proofTemp;
    free((void*)proofTemp);
  } else {
    return "";
  }

  return proof;
}

void BatContribution::ReconcilePublisherList(
    ledger::PUBLISHER_CATEGORY category,
    const ledger::PublisherInfoList& list,
    uint32_t next_record) {
  std::vector<braveledger_bat_helper::PUBLISHER_ST> newList;
  for (const auto &publisher : list) {
    braveledger_bat_helper::PUBLISHER_ST new_publisher;
    new_publisher.id_ = publisher.id;
    new_publisher.percent_ = publisher.percent;
    new_publisher.weight_ = publisher.weight;
    new_publisher.duration_ = publisher.duration;
    new_publisher.score_ = publisher.score;
    new_publisher.visits_ = publisher.visits;
    newList.push_back(new_publisher);
  }

  Reconcile(ledger_->GenerateGUID(), category, newList);
}

void BatContribution::OnTimerReconcile() {
  ledger_->GetRecurringDonations(
      std::bind(&BatContribution::ReconcilePublisherList,
                this,
                ledger::PUBLISHER_CATEGORY::RECURRING_DONATION,
                std::placeholders::_1,
                std::placeholders::_2));
}

void BatContribution::StartAutoContribute() {
  uint64_t currentReconcileStamp = ledger_->GetReconcileStamp();
  ledger::PublisherInfoFilter filter = ledger_->CreatePublisherFilter(
      "",
      ledger::PUBLISHER_CATEGORY::AUTO_CONTRIBUTE,
      ledger::PUBLISHER_MONTH::ANY,
      -1,
      ledger::PUBLISHER_EXCLUDE_FILTER::FILTER_DEFAULT,
      true,
      currentReconcileStamp);
  ledger_->GetCurrentPublisherInfoList(
      0,
      0,
      filter,
      std::bind(&BatContribution::ReconcilePublisherList,
                this,
                ledger::PUBLISHER_CATEGORY::AUTO_CONTRIBUTE,
                std::placeholders::_1,
                std::placeholders::_2));
}

void BatContribution::Reconcile(
    const std::string& viewingId,
    const ledger::PUBLISHER_CATEGORY category,
    const braveledger_bat_helper::PublisherList& list,
    const braveledger_bat_helper::Directions& directions) {
  if (ledger_->ReconcileExists(viewingId)) {
    ledger_->Log(__func__,
                 ledger::LogLevel::LOG_ERROR,
                 {"unable to reconcile with the same viewing id"});
    // TODO(nejczdovc) add error callback
    return;
  }

  auto reconcile = braveledger_bat_helper::CURRENT_RECONCILE();

  double fee = .0;

  double balance = ledger_->GetBalance();

  if (category == ledger::PUBLISHER_CATEGORY ::AUTO_CONTRIBUTE) {
    double ac_amount = ledger_->GetContributionAmount();

    if (list.size() == 0 || ac_amount > balance) {
      if (list.size() == 0) {
        ledger_->Log(__func__,
                     ledger::LogLevel::LOG_INFO,
                     {"AC table is empty"});
      }

      if (ac_amount > balance) {
        ledger_->Log(__func__,
                     ledger::LogLevel::LOG_INFO,
                     {"You don't have enough funds for AC contribution"});
      }

      ledger_->ResetReconcileStamp();
      // TODO(nejczdovc) add error callback
      return;
    }

    reconcile.list_ = list;
  }

  if (category == ledger::PUBLISHER_CATEGORY::RECURRING_DONATION) {
    double ac_amount = ledger_->GetContributionAmount();
    if (list.size() == 0) {
      ledger_->Log(__func__,
                   ledger::LogLevel::LOG_INFO,
                   {"recurring donation list is empty"});
      StartAutoContribute();
      // TODO(nejczdovc) add error callback
      return;
    }

    for (const auto& publisher : list) {
      if (publisher.id_.empty()) {
        ledger_->Log(__func__,
                     ledger::LogLevel::LOG_ERROR,
                     {"recurring donation is missing publisher"});
        StartAutoContribute();
        // TODO(nejczdovc) add error callback
        return;
      }

      fee += publisher.weight_;
    }

    if (fee + ac_amount > balance) {
        ledger_->Log(__func__,
                     ledger::LogLevel::LOG_ERROR,
                     {"You don't have enough funds to "
                      "do recurring and AC contribution"});
      // TODO(nejczdovc) add error callback
      return;
    }

    reconcile.list_ = list;
  }

  if (category == ledger::PUBLISHER_CATEGORY::DIRECT_DONATION) {
    for (const auto& direction : directions) {
      if (direction.publisher_key_.empty()) {
        ledger_->Log(__func__,
                     ledger::LogLevel::LOG_ERROR,
                     {"reconcile direction missing publisher"});
        // TODO(nejczdovc) add error callback
        return;
      }

      if (direction.currency_ != CURRENCY) {
        ledger_->Log(__func__,
                     ledger::LogLevel::LOG_ERROR,
                     {"reconcile direction currency invalid for ",
                      direction.publisher_key_});
        // TODO(nejczdovc) add error callback
        return;
      }

      fee += direction.amount_;
    }

    if (fee > balance) {
      ledger_->Log(__func__,
                   ledger::LogLevel::LOG_ERROR,
                   {"You don't have enough funds to do a tip"});
      // TODO(nejczdovc) add error callback
      return;
    }
  }

  reconcile.viewingId_ = viewingId;
  reconcile.fee_ = fee;
  reconcile.directions_ = directions;
  reconcile.category_ = category;

  ledger_->AddReconcile(viewingId, reconcile);

  std::string url = braveledger_bat_helper::buildURL(
      (std::string)RECONCILE_CONTRIBUTION + ledger_->GetUserId(), PREFIX_V2);
  auto request_id = ledger_->LoadURL(url,
      std::vector<std::string>(), "", "",
      ledger::URL_METHOD::GET,
      &handler_);

  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatContribution::ReconcileCallback,
                                       this,
                                       viewingId,
                                       std::placeholders::_1,
                                       std::placeholders::_2,
                                       std::placeholders::_3));
}

void BatContribution::ReconcileCallback(
    const std::string& viewingId,
    bool result,
    const std::string& response,
    const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  auto reconcile = ledger_->GetReconcileById(viewingId);

  if (!result || reconcile.viewingId_.empty()) {
    // TODO(nejczdovc) errors handling
    return;
  }

  braveledger_bat_helper::getJSONValue(SURVEYOR_ID,
                                       response,
                                       reconcile.surveyorInfo_.surveyorId_);
  bool success = ledger_->UpdateReconcile(reconcile);
  if (!success) {
    // TODO(nejczdovc) error handling
    return;
  }

  CurrentReconcile(viewingId);
}

void BatContribution::CurrentReconcile(const std::string& viewingId) {
  std::ostringstream amount;
  auto reconcile = ledger_->GetReconcileById(viewingId);

  if (reconcile.category_ == ledger::PUBLISHER_CATEGORY::AUTO_CONTRIBUTE) {
    amount << ledger_->GetContributionAmount();
  } else {
    amount << reconcile.fee_;
  }

  std::string currency = ledger_->GetCurrency();
  std::string path = (std::string)WALLET_PROPERTIES +
                      ledger_->GetPaymentId() +
                      "?refresh=true" +
                      "&amount=" +
                      amount.str() +
                      "&altcurrency=" +
                      currency;

  auto request_id = ledger_->LoadURL(
      braveledger_bat_helper::buildURL(path, PREFIX_V2),
      std::vector<std::string>(), "", "",
      ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(
                                 &BatContribution::CurrentReconcileCallback,
                                 this,
                                 viewingId,
                                 std::placeholders::_1,
                                 std::placeholders::_2,
                                 std::placeholders::_3));
}

void BatContribution::CurrentReconcileCallback(
    const std::string& viewingId,
    bool result,
    const std::string& response,
    const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  if (!result) {
    ledger_->OnReconcileComplete(ledger::Result::LEDGER_ERROR, viewingId);
    // TODO(nejczdovc) errors handling
    return;
  }

  auto reconcile = ledger_->GetReconcileById(viewingId);

  braveledger_bat_helper::getJSONRates(response, reconcile.rates_);
  braveledger_bat_helper::UNSIGNED_TX unsignedTx;
  braveledger_bat_helper::getJSONUnsignedTx(response, unsignedTx);

  if (unsignedTx.amount_.empty() &&
      unsignedTx.currency_.empty() &&
      unsignedTx.destination_.empty()) {
    ledger_->OnReconcileComplete(
        ledger::Result::LEDGER_ERROR, reconcile.viewingId_);
    // We don't have any unsigned transactions
    // TODO(nejczdovc) error handling
    return;
  }

  reconcile.amount_ = unsignedTx.amount_;
  reconcile.currency_ = unsignedTx.currency_;
  bool success = ledger_->UpdateReconcile(reconcile);

  if (!success) {
    // TODO(nejczdovc) error handling
    return;
  }

  braveledger_bat_helper::WALLET_INFO_ST wallet_info = ledger_->GetWalletInfo();
  std::string octets = braveledger_bat_helper::stringifyUnsignedTx(unsignedTx);

  std::string headerDigest = "SHA-256=" +
      braveledger_bat_helper::getBase64(
          braveledger_bat_helper::getSHA256(octets));

  std::string headerKeys[1] = {"digest"};
  std::string headerValues[1] = {headerDigest};

  std::vector<uint8_t> secretKey = braveledger_bat_helper::getHKDF(
      wallet_info.keyInfoSeed_);
  std::vector<uint8_t> publicKey;
  std::vector<uint8_t> newSecretKey;
  braveledger_bat_helper::getPublicKeyFromSeed(secretKey,
                                               publicKey,
                                               newSecretKey);
  std::string headerSignature = braveledger_bat_helper::sign(headerKeys,
                                                             headerValues,
                                                             1,
                                                             "primary",
                                                             newSecretKey);

  braveledger_bat_helper::RECONCILE_PAYLOAD_ST reconcilePayload;
  reconcilePayload.requestType_ = "httpSignature";
  reconcilePayload.request_signedtx_headers_digest_ = headerDigest;
  reconcilePayload.request_signedtx_headers_signature_ = headerSignature;
  reconcilePayload.request_signedtx_body_ = unsignedTx;
  reconcilePayload.request_signedtx_octets_ = octets;
  reconcilePayload.request_viewingId_ = reconcile.viewingId_;
  reconcilePayload.request_surveyorId_ = reconcile.surveyorInfo_.surveyorId_;
  std::string payloadStringify =
      braveledger_bat_helper::stringifyReconcilePayloadSt(reconcilePayload);

  std::vector<std::string> walletHeader;
  walletHeader.push_back("Content-Type: application/json; charset=UTF-8");
  std::string path = (std::string)WALLET_PROPERTIES + ledger_->GetPaymentId();

  auto request_id = ledger_->LoadURL(
      braveledger_bat_helper::buildURL(path, PREFIX_V2),
      walletHeader, payloadStringify, "application/json; charset=utf-8",
      ledger::URL_METHOD::PUT,
      &handler_);
  handler_.AddRequestHandler(
      std::move(request_id),
      std::bind(&BatContribution::ReconcilePayloadCallback,
               this,
               viewingId,
               std::placeholders::_1,
               std::placeholders::_2,
               std::placeholders::_3));
}

void BatContribution::ReconcilePayloadCallback(
    const std::string& viewingId,
    bool result,
    const std::string& response,
    const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  if (!result) {
    ledger_->OnReconcileComplete(ledger::Result::LEDGER_ERROR, viewingId);
    // TODO(nejczdovc) errors handling
    return;
  }

  const auto reconcile = ledger_->GetReconcileById(viewingId);

  braveledger_bat_helper::TRANSACTION_ST transaction;
  braveledger_bat_helper::getJSONTransaction(response, transaction);
  transaction.viewingId_ = reconcile.viewingId_;
  transaction.surveyorId_ = reconcile.surveyorInfo_.surveyorId_;
  transaction.contribution_rates_ = reconcile.rates_;
  transaction.contribution_fiat_amount_ = reconcile.amount_;
  transaction.contribution_fiat_currency_ = reconcile.currency_;

  braveledger_bat_helper::Transactions transactions =
      ledger_->GetTransactions();
  transactions.push_back(transaction);
  ledger_->SetTransactions(transactions);
  RegisterViewing(viewingId);
}

void BatContribution::RegisterViewing(const std::string& viewingId) {
  auto request_id = ledger_->LoadURL(
      braveledger_bat_helper::buildURL((std::string)REGISTER_VIEWING, PREFIX_V2),
      std::vector<std::string>(),
      "",
      "",
      ledger::URL_METHOD::GET, &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(&BatContribution::RegisterViewingCallback,
                                       this,
                                       viewingId,
                                       std::placeholders::_1,
                                       std::placeholders::_2,
                                       std::placeholders::_3));
}

void BatContribution::RegisterViewingCallback(
    const std::string& viewingId,
    bool result,
    const std::string& response,
    const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  if (!result) {
    ledger_->OnReconcileComplete(ledger::Result::LEDGER_ERROR, viewingId);
    // TODO(nejczdovc) errors handling
    return;
  }

  auto reconcile = ledger_->GetReconcileById(viewingId);

  braveledger_bat_helper::getJSONValue(REGISTRARVK_FIELDNAME,
                                       response,
                                       reconcile.registrarVK_);
  DCHECK(!reconcile.registrarVK_.empty());
  reconcile.anonizeViewingId_ = reconcile.viewingId_;
  reconcile.anonizeViewingId_.erase(
      std::remove(reconcile.anonizeViewingId_.begin(),
                  reconcile.anonizeViewingId_.end(),
                  '-'),
      reconcile.anonizeViewingId_.end());
  reconcile.anonizeViewingId_.erase(12, 1);

  std::string proof = GetAnonizeProof(reconcile.registrarVK_,
                                      reconcile.anonizeViewingId_,
                                      reconcile.preFlight_);

  bool success = ledger_->UpdateReconcile(reconcile);
  if (!success) {
    // TODO(nejczdovc) error handling
    return;
  }

  std::string keys[1] = {"proof"};
  std::string values[1] = {proof};
  std::string proofStringified = braveledger_bat_helper::stringify(keys,
                                                                   values,
                                                                   1);
  ViewingCredentials(viewingId, proofStringified, reconcile.anonizeViewingId_);
}

void BatContribution::ViewingCredentials(const std::string& viewingId,
                                         const std::string& proofStringified,
                                         const std::string& anonizeViewingId) {
  std::string url = braveledger_bat_helper::buildURL(
      (std::string)REGISTER_VIEWING +
      "/" +
      anonizeViewingId, PREFIX_V2);

  auto request_id = ledger_->LoadURL(url,
                                     std::vector<std::string>(),
                                     proofStringified,
                                     "application/json; charset=utf-8",
                                     ledger::URL_METHOD::POST,
                                     &handler_);
  handler_.AddRequestHandler(std::move(request_id),
                             std::bind(
                                 &BatContribution::ViewingCredentialsCallback,
                                 this,
                                 viewingId,
                                 std::placeholders::_1,
                                 std::placeholders::_2,
                                 std::placeholders::_3));
}

void BatContribution::ViewingCredentialsCallback(
    const std::string& viewingId,
    bool result,
    const std::string& response,
    const std::map<std::string, std::string>& headers) {
  ledger_->LogResponse(__func__, result, response, headers);

  if (!result) {
    ledger_->OnReconcileComplete(ledger::Result::LEDGER_ERROR, viewingId);
    // TODO(nejczdovc) errors handling
    return;
  }

  auto reconcile = ledger_->GetReconcileById(viewingId);

  std::string verification;
  braveledger_bat_helper::getJSONValue(VERIFICATION_FIELDNAME,
                                       response,
                                       verification);
  const char* masterUserToken = registerUserFinal(
      reconcile.anonizeViewingId_.c_str(),
      verification.c_str(),
      reconcile.preFlight_.c_str(),
      reconcile.registrarVK_.c_str());

  if (nullptr != masterUserToken) {
    reconcile.masterUserToken_ = masterUserToken;
    free((void*)masterUserToken);
  }

  bool success = ledger_->UpdateReconcile(reconcile);
  if (!success) {
    // TODO(nejczdovc) error handling
    return;
  }

  std::vector<std::string> surveyors;
  braveledger_bat_helper::getJSONList(SURVEYOR_IDS, response, surveyors);
  std::string probi = "0";
  // Save the rest values to transactions
  braveledger_bat_helper::Transactions transactions =
      ledger_->GetTransactions();

  for (size_t i = 0; i < transactions.size(); i++) {
    if (transactions[i].viewingId_ != reconcile.viewingId_) {
      continue;
    }

    transactions[i].anonizeViewingId_ = reconcile.anonizeViewingId_;
    transactions[i].registrarVK_ = reconcile.registrarVK_;
    transactions[i].masterUserToken_ = reconcile.masterUserToken_;
    transactions[i].surveyorIds_ = surveyors;
    probi = transactions[i].contribution_probi_;
  }

  ledger_->SetTransactions(transactions);
  ledger_->OnReconcileComplete(ledger::Result::LEDGER_OK,
                               reconcile.viewingId_,
                               probi);
}

unsigned int BatContribution::GetBallotsCount(const std::string& viewingId) {
  unsigned int count = 0;
  braveledger_bat_helper::Transactions transactions =
      ledger_->GetTransactions();
  for (size_t i = 0; i < transactions.size(); i++) {
    if (transactions[i].votes_ < transactions[i].surveyorIds_.size()
        && transactions[i].viewingId_ == viewingId) {
      count += transactions[i].surveyorIds_.size() - transactions[i].votes_;
    }
  }

  return count;
}

void BatContribution::OnReconcileCompleteSuccess(
    const std::string& viewing_id,
    ledger::PUBLISHER_CATEGORY category,
    const std::string& probi,
    ledger::PUBLISHER_MONTH month,
    int year,
    uint32_t date) {
  if (category == ledger::PUBLISHER_CATEGORY::AUTO_CONTRIBUTE) {
    ledger_->SetBalanceReportItem(month,
                                  year,
                                  ledger::ReportType::AUTO_CONTRIBUTION,
                                  probi);
    ledger_->SaveContributionInfo(probi, month, year, date, "", category);
    return;
  }

  if (category == ledger::PUBLISHER_CATEGORY::DIRECT_DONATION) {
    ledger_->SetBalanceReportItem(month,
                                  year,
                                  ledger::ReportType::DONATION,
                                  probi);
    auto reconcile = ledger_->GetReconcileById(viewing_id);
    auto donations = reconcile.directions_;
    if (donations.size() > 0) {
      std::string publisher_key = donations[0].publisher_key_;
      ledger_->SaveContributionInfo(probi,
                                    month,
                                    year,
                                    date,
                                    publisher_key,
                                    category);
    }
    return;
  }

  if (category == ledger::PUBLISHER_CATEGORY::RECURRING_DONATION) {
    auto reconcile = ledger_->GetReconcileById(viewing_id);
    ledger_->SetBalanceReportItem(month,
                                  year,
                                  ledger::ReportType::DONATION_RECURRING,
                                  probi);
    for (auto &publisher : reconcile.list_) {
      // TODO(nejczdovc) remove when we completely switch to probi
      const std::string probi =
          std::to_string(static_cast<int>(publisher.weight_)) +
          "000000000000000000";
      ledger_->SaveContributionInfo(probi,
                                    month,
                                    year,
                                    date,
                                    publisher.id_,
                                    category);
    }
    return;
  }
}

}  // namespace braveledger_bat_contribution