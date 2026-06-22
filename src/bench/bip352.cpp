// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <common/bip352.h>

struct BIP352Fixture {
    ECC_Context ecc_context;
    CKey scan_key;
    CPubKey spend_pubkey;
    bip352::PrevoutsSummary prevouts_summary;
    // outputs[i] is addressed to this recipient
    std::vector<XOnlyPubKey> outputs_matching;
    // one output that will never match (random key), used for the no-match path
    std::vector<XOnlyPubKey> outputs_nonmatching;
    std::unordered_map<CKeyID, uint256, SaltedSipHasher> labels_cache;
    std::map<CPubKey, uint256> labels_cache_ordered;

    explicit BIP352Fixture(size_t n_outputs, size_t n_labels)
    {
        CKey sender_key;
        sender_key.MakeNewKey(true);
        CPubKey sender_pubkey{sender_key.GetPubKey()};

        CKey spend_key;
        scan_key.MakeNewKey(true);
        spend_key.MakeNewKey(true);
        spend_pubkey = spend_key.GetPubKey();

        COutPoint outpoint{Txid::FromHex("0000000000000000000000000000000000000000000000000000000000000001").value(), 0};
        CTxIn txin{outpoint};
        txin.scriptWitness.stack.emplace_back();
        txin.scriptWitness.stack.emplace_back(sender_pubkey.begin(), sender_pubkey.end());

        std::map<COutPoint, Coin> coins;
        coins[outpoint] = Coin{CTxOut{{}, GetScriptForDestination(WitnessV0KeyHash{sender_pubkey})}, 0, false};

        auto summary = bip352::GetSilentPaymentsPrevoutsSummary({txin}, coins);
        assert(summary.has_value());
        prevouts_summary = std::move(*summary);

        // Generate n_outputs matching outputs in a single call so the ECDH counter
        // increments correctly (output k uses hash(shared_secret || k)).
        if (n_outputs > 0) {
            std::map<size_t, V0SilentPaymentsDestination> sp_dests;
            for (size_t i = 0; i < n_outputs; ++i) {
                sp_dests.emplace(i, V0SilentPaymentsDestination{scan_key.GetPubKey(), spend_pubkey});
            }
            auto tr_dests = bip352::GenerateSilentPaymentsTaprootDestinations(sp_dests, {sender_key}, {}, outpoint);
            assert(tr_dests.has_value());
            for (size_t i = 0; i < n_outputs; ++i) {
                outputs_matching.emplace_back(tr_dests->at(i));
            }
        }

        // One non-matching output (random key, will never match the recipient)
        CKey dummy;
        dummy.MakeNewKey(true);
        outputs_nonmatching.emplace_back(dummy.GetPubKey());

        // Build both cache variants from the same labels
        labels_cache.reserve(n_labels);
        for (size_t i = 0; i < n_labels; ++i) {
            auto [label_pubkey, label_tweak] = bip352::CreateLabel(scan_key, i);
            labels_cache.emplace(label_pubkey.GetID(), label_tweak);
            labels_cache_ordered.emplace(label_pubkey, label_tweak);
        }
    }
};

// Baseline: 1 output, no labels. Measures pure ECDH + single output check cost.
static void BIP352Scan1OutputNoLabels(benchmark::Bench& bench)
{
    BIP352Fixture f{1, 0};
    bench.batch(1).run([&] {
        auto res = bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_matching, f.labels_cache);
        assert(res.has_value());
    });
}

// 1 output, 100k labels in cache. Shows labels cache overhead vs baseline.
static void BIP352Scan1OutputWithLabels(benchmark::Bench& bench)
{
    BIP352Fixture f{1, 100'001};
    bench.batch(1).run([&] {
        auto res = bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_matching, f.labels_cache);
        assert(res.has_value());
    });
}

// 10 outputs per tx, no labels. Batch-normalised to ns/output so you can see
// how much each additional output costs beyond the fixed ECDH.
static void BIP352Scan10OutputsNoLabels(benchmark::Bench& bench)
{
    BIP352Fixture f{10, 0};
    bench.batch(10).run([&] {
        auto res = bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_matching, f.labels_cache);
        assert(res.has_value());
    });
}

// No-match path, no labels: ECDH + 1 EC check, no hashtable involved.
// Baseline for measuring the marginal cost that labels add during scanning.
static void BIP352ScanNoMatchNoLabels(benchmark::Bench& bench)
{
    BIP352Fixture f{0, 0};
    bench.batch(1).run([&] {
        bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_nonmatching, f.labels_cache);
    });
}

// No-match path with 1k labels. Compare ns/op to NoLabels and 100k variants:
// (ns_with_N - ns_no_labels) / N gives the amortised cost per label entry.
static void BIP352ScanNoMatchWith1kLabels(benchmark::Bench& bench)
{
    BIP352Fixture f{0, 1'000};
    bench.batch(1).run([&] {
        bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_nonmatching, f.labels_cache);
    });
}

// No-match path with 100k labels. Common wallet label ceiling.
static void BIP352ScanNoMatchWith100kLabels(benchmark::Bench& bench)
{
    BIP352Fixture f{0, 100'001};
    bench.batch(1).run([&] {
        bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_nonmatching, f.labels_cache);
    });
}

// std::map<CPubKey> variants: binary search on raw pubkey bytes, no Hash160.
static void BIP352ScanNoMatchWith1kLabels_OrderedMap(benchmark::Bench& bench)
{
    BIP352Fixture f{0, 1'000};
    bench.batch(1).run([&] {
        bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_nonmatching, f.labels_cache_ordered);
    });
}

static void BIP352ScanNoMatchWith100kLabels_OrderedMap(benchmark::Bench& bench)
{
    BIP352Fixture f{0, 100'001};
    bench.batch(1).run([&] {
        bip352::ScanForSilentPaymentsOutputs(
            f.scan_key, f.prevouts_summary, f.spend_pubkey,
            f.outputs_nonmatching, f.labels_cache_ordered);
    });
}

BENCHMARK(BIP352Scan1OutputNoLabels);
BENCHMARK(BIP352Scan1OutputWithLabels);
BENCHMARK(BIP352Scan10OutputsNoLabels);
BENCHMARK(BIP352ScanNoMatchNoLabels);
BENCHMARK(BIP352ScanNoMatchWith1kLabels);
BENCHMARK(BIP352ScanNoMatchWith1kLabels_OrderedMap);
BENCHMARK(BIP352ScanNoMatchWith100kLabels);
BENCHMARK(BIP352ScanNoMatchWith100kLabels_OrderedMap);
