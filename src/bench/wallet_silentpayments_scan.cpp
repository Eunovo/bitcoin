// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <bench/bench.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <sync.h>
#include <validation.h>
#include <wallet/test/util.h>
#include <wallet/receive.h>
#include <wallet/wallet.h>

#include <vector>

namespace wallet {

static int SATS_PER_OUTPUT = 1;
static int FEE_SATS = 1000;
static size_t SP_RECIPIENT_GROUP_LIMIT = 1000;

static void WalletSPScan(benchmark::Bench& bench, size_t num_txs, size_t outputs_per_tx, std::optional<size_t> n_payments = {})
{
    auto testsetup = TestChain100Setup();
    auto& m_node = testsetup.m_node;
    CKey coinbaseKey = testsetup.coinbaseKey;
    CScript coinbaseScript = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    WitnessV1Taproot spenddest{XOnlyPubKey(coinbaseKey.GetPubKey())};
    CScript spendScript = GetScriptForDestination(spenddest);
    CBlockIndex* startingTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    FlatSigningProvider keystore;
    keystore.keys.emplace(coinbaseKey.GetPubKey().GetID(), coinbaseKey);
    keystore.pubkeys.emplace(coinbaseKey.GetPubKey().GetID(), coinbaseKey.GetPubKey());

    int SATS_PER_INPUT = (outputs_per_tx * SATS_PER_OUTPUT) + FEE_SATS;
    // We need to ensure enough sats for the outputs
    assert((SATS_PER_INPUT * num_txs) < 50 * COIN);

    int nHeight = startingTip->nHeight;
    CMutableTransaction funding_tx;
    for (size_t i = 0; i < num_txs; ++i) {
        funding_tx.vout.emplace_back(SATS_PER_INPUT, spendScript);
    }
    funding_tx.vin.emplace_back(testsetup.m_coinbase_txns[0]->GetHash(), 0);
    std::map<int, bilingual_str> errors;

    assert(SignTransaction(
        funding_tx,
        &keystore,
        {{COutPoint(testsetup.m_coinbase_txns[0]->GetHash(), 0),
            Coin(CTxOut(50 * COIN, coinbaseScript), 1, true)}},
        SIGHASH_ALL,
        errors));

    auto block = testsetup.CreateAndProcessBlock({funding_tx}, coinbaseScript);
    nHeight++;
    auto funding_txid = block.vtx[1]->GetHash();
    Coin coin(CTxOut{SATS_PER_INPUT, spendScript}, nHeight, false);

    CBlockIndex* currentTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    assert(nHeight == currentTip->nHeight);

    CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet.cs_wallet);
        LOCK(Assert(m_node.chainman)->GetMutex());
        wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet.SetWalletFlag(WALLET_FLAG_SILENT_PAYMENTS);
        wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        wallet.SetupDescriptorScriptPubKeyMans();
    }
    auto spdest = wallet.GetNewDestination(OutputType::SILENT_PAYMENTS, "test");
    assert(spdest.has_value());

    const size_t num_sp_outputs = n_payments ?
     std::min(SP_RECIPIENT_GROUP_LIMIT, *n_payments) :
     std::min(SP_RECIPIENT_GROUP_LIMIT, outputs_per_tx);
    assert(num_sp_outputs <= outputs_per_tx); // n_payments must not be more that outputs_per_tx
    std::map<size_t, V0SilentPaymentDestination> dests;
    for (size_t i = 0; i < num_sp_outputs; ++i) {
        dests.emplace(i, std::get<V0SilentPaymentDestination>(*spdest));
    }

    std::vector<CMutableTransaction> txs;
    txs.reserve(num_txs);

    for (size_t i = 0; i < num_txs; i++) {
        COutPoint outpoint{funding_txid, i};
        std::optional<std::map<size_t, WitnessV1Taproot>> outputSpks;
        if (dests.size() > 0) {
            outputSpks = bip352::GenerateSilentPaymentTaprootDestinations(
                dests,
                /*plain_keys=*/{},
                /*taproot_keys=*/{testsetup.coinbaseKey.ComputeKeyPair(nullptr)},
                /*smallest_outpoint=*/outpoint);
        }

        CMutableTransaction tx;
        tx.vin.emplace_back(outpoint);
        // Add outputs in reverse order
        for (size_t i = outputs_per_tx; i > 0; i--) {
            WitnessV1Taproot output;
            if (i <= num_sp_outputs) output = outputSpks->at(i-1);
            else output = spenddest;
            tx.vout.emplace_back(SATS_PER_OUTPUT, GetScriptForDestination(output));
        }
        std::map<int, bilingual_str> errors;
        assert(SignTransaction(tx, &keystore, {{outpoint, coin}}, SIGHASH_ALL, errors));
        txs.push_back(std::move(tx));
    }

    testsetup.CreateAndProcessBlock(txs, coinbaseScript);
    currentTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    assert(currentTip->nHeight == nHeight + 1);

    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash()));
    {
        CBlockLocator locator;
        assert(WalletBatch{wallet.GetDatabase()}.ReadBestBlock(locator));
        assert(!locator.IsNull() && locator.vHave.front() == currentTip->GetBlockHash());
    }

    assert(GetBalance(wallet).m_mine_trusted == 0);
    int expected_balance = SATS_PER_OUTPUT * num_sp_outputs * num_txs;

    bench.epochs(1).epochIterations(1).unit("block").run([&] {
        WalletRescanReserver reserver(wallet);
        reserver.reserve();
        auto result = wallet.ScanForWalletTransactions(
            /*start_block=*/currentTip->GetBlockHash(),
            /*start_height=*/currentTip->nHeight,
            /*max_height=*/{},
            reserver,
            /*fUpdate=*/false,
            /*save_progress=*/true);
        assert(result.status == CWallet::ScanResult::SUCCESS);
        assert(result.last_scanned_height == currentTip->nHeight);
    });
    assert(GetBalance(wallet).m_mine_trusted == expected_balance);
}

/**
 * Max Txs with a PKHash input is 4849
 * Max Txs with a WitnessV0KeyHash input is 8245
 * Max Txs with a WitnessV1Taproot input is 8986
 */
static void WalletSPScanMaxTxOneOutput(benchmark::Bench& bench) { WalletSPScan(bench, 8986, 1); }
static void WalletSPScanOneTxMaxOutputs(benchmark::Bench& bench) { WalletSPScan(bench, 1, 23246); }
static void WalletSPScanAvgTxAvgOutputs(benchmark::Bench& bench) { WalletSPScan(bench, 3200, 3); }
static void WalletSPScanNoPayments(benchmark::Bench& bench) { WalletSPScan(bench, 1, 1000, 0); }
static void WalletSPScanTenPayments(benchmark::Bench& bench) { WalletSPScan(bench, 1, 1000, 10); }
BENCHMARK(WalletSPScanMaxTxOneOutput);
BENCHMARK(WalletSPScanOneTxMaxOutputs);
BENCHMARK(WalletSPScanAvgTxAvgOutputs);
BENCHMARK(WalletSPScanNoPayments);
BENCHMARK(WalletSPScanTenPayments);
} // namespace wallet
