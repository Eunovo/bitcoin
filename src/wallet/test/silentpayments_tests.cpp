// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <coins.h>
#include <common/bip352.h>
#include <node/blockstorage.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <sync.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <util/result.h>
#include <validation.h>
#include <wallet/receive.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>
#include <stdio.h>

using node::MAX_BLOCKFILE_SIZE;

namespace wallet {

BOOST_FIXTURE_TEST_SUITE(wallet_silentpayments_tests, WalletTestingSetup)

BOOST_FIXTURE_TEST_CASE(silentpayments_scan_test, TestChain100Setup)
{
    PKHash spenddest{coinbaseKey.GetPubKey()};
    CScript spendScript = GetScriptForDestination(spenddest);
    CBlockIndex* prevTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());

    auto block = CreateAndProcessBlock({}, spendScript);
    int spendHeight = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Height());
    Assert(prevTip->nHeight + 1 == spendHeight);
    COutPoint spendOutpoint{block.vtx[0]->GetHash(), 0};
    mineBlocks(100);

    // Cap last block file size, and mine new block in a new block file.
    prevTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    WITH_LOCK(::cs_main, m_node.chainman->m_blockman.GetBlockFileInfo(prevTip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);

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
    BOOST_CHECK(spdest.has_value());

    const size_t NUM_OUTPUTS = 10; // 23246 outputs is the max that will fit in this block
    // 1000 sats per output, allows up to 5000000 outputs to fully spend the coinbase
    assert(NUM_OUTPUTS < 5000000);

    std::map<size_t, V0SilentPaymentDestination> dests;
    for (size_t i = 0; i < NUM_OUTPUTS; ++i) {
        dests.emplace(i, std::get<V0SilentPaymentDestination>(*spdest));
    }

    auto outputSpks = bip352::GenerateSilentPaymentTaprootDestinations(
        dests,
        /*plain_keys=*/{coinbaseKey},
        /*taproot_keys=*/{},
        /*smallest_outpoint=*/spendOutpoint);
    BOOST_CHECK(outputSpks.has_value());
    BOOST_CHECK(outputSpks->size() == NUM_OUTPUTS);

    CMutableTransaction tx;
    tx.vin.emplace_back(spendOutpoint);
    for (const auto& [_, spk] : *outputSpks) {
        // 1000 sats per output, allows up to 5000000 outputs to fully spend the coinbase
        tx.vout.emplace_back(1000, GetScriptForDestination(spk));
    }
    std::map<int, bilingual_str> input_errors;
    std::map<COutPoint, Coin> coins;
    coins.emplace(
        spendOutpoint,
        Coin(CTxOut(50 * COIN, spendScript), spendHeight, true));
    FlatSigningProvider keystore;
    keystore.keys.emplace(coinbaseKey.GetPubKey().GetID(), coinbaseKey);
    keystore.pubkeys.emplace(coinbaseKey.GetPubKey().GetID(), coinbaseKey.GetPubKey());
    BOOST_CHECK(SignTransaction(tx, &keystore, coins, SIGHASH_ALL, input_errors));

    CreateAndProcessBlock({tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    CBlockIndex* newTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    BOOST_CHECK(newTip->nHeight == prevTip->nHeight + 1);

    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash()));
    {
        CBlockLocator locator;
        BOOST_CHECK(WalletBatch{wallet.GetDatabase()}.ReadBestBlock(locator));
        BOOST_CHECK(!locator.IsNull() && locator.vHave.front() == newTip->GetBlockHash());
    }

    {
        BOOST_CHECK(GetBalance(wallet).m_mine_trusted == 0);
        WalletRescanReserver reserver(wallet);
        BOOST_CHECK(reserver.reserve());
        auto result = wallet.ScanForWalletTransactions(
            /*start_block=*/newTip->GetBlockHash(),
            /*start_height=*/newTip->nHeight,
            /*max_height=*/{},
            reserver,
            /*fUpdate=*/false,
            /*save_progress=*/true);
        BOOST_CHECK(result.status == CWallet::ScanResult::SUCCESS);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK(result.last_scanned_height.has_value());
        BOOST_CHECK(*result.last_scanned_height == newTip->nHeight);

        BOOST_CHECK(GetBalance(wallet).m_mine_untrusted_pending == 0);
        BOOST_CHECK(GetBalance(wallet).m_mine_immature == 0);
        BOOST_CHECK(GetBalance(wallet).m_mine_trusted == 1000 * NUM_OUTPUTS);
    }
}
BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet