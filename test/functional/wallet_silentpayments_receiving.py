#!/usr/bin/env python3

from test_framework.descriptors import descsum_create
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_approx,
    assert_equal,
    assert_raises_rpc_error,
)


class SilentPaymentsReceivingTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)
        parser.add_argument("-maxtxfee", dest="maxtxfee", default=10000)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def test_encrypt_and_decrypt(self):
        self.log.info("Check that a silent payments wallet can be encrypted and decrypted")
        self.log.info("Create encrypted wallet")
        self.nodes[0].createwallet(wallet_name="sp_encrypted", passphrase="unsigned integer", silent_payment=True)
        wallet = self.nodes[0].get_wallet_rpc("sp_encrypted")
        addr = wallet.getnewaddress(address_type="silent-payment")
        self.def_wallet.sendtoaddress(addr, 10)
        self.generate(self.nodes[0], 1)
        self.log.info("Check that we can scan without the wallet being unlocked")
        assert_equal(wallet.getbalance(), 10)
        self.log.info("Check that we get an error if trying to send with the wallet locked")
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", wallet.sendtoaddress, addr, 9)
        wallet.walletpassphrase(passphrase="unsigned integer", timeout=3)
        self.log.info("Unlock wallet and send")
        wallet.sendtoaddress(addr, 9)
        self.generate(self.nodes[0], 1)
        assert_approx(wallet.getbalance(), 10, 0.0001)

    def test_encrypting_unencrypted(self):
        self.log.info("Check that a silent payments wallet can be encrypted after creation")
        self.log.info("Create un-encrypted wallet")
        self.nodes[0].createwallet(wallet_name="sp_unencrypted", silent_payment=True)
        wallet = self.nodes[0].get_wallet_rpc("sp_unencrypted")
        addr = wallet.getnewaddress(address_type="silent-payment")
        self.def_wallet.sendtoaddress(addr, 10)
        self.generate(self.nodes[0], 1)
        assert_equal(wallet.getbalance(), 10)
        self.log.info("Add a passphrase to the wallet")
        wallet.encryptwallet(passphrase="unsigned integer")
        self.log.info("Check that we get an error if trying to send with the wallet locked")
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", wallet.sendtoaddress, addr, 9)
        wallet.walletpassphrase(passphrase="unsigned integer", timeout=3)
        self.log.info("Unlock wallet and send")
        wallet.sendtoaddress(addr, 9)
        self.generate(self.nodes[0], 1)
        assert_approx(wallet.getbalance(), 10, 0.0001)

    def test_createwallet(self):
        self.log.info("Check createwallet silent payments option")

        self.nodes[0].createwallet(wallet_name="sp", silent_payment=True)
        wallet = self.nodes[0].get_wallet_rpc("sp")
        addr = wallet.getnewaddress(address_type="silent-payment")
        assert addr.startswith("sp")

        self.nodes[0].createwallet(wallet_name="non_sp", silent_payment=False)
        wallet = self.nodes[0].get_wallet_rpc("non_sp")
        assert_raises_rpc_error(-12, "Error: No silent-payment addresses available", wallet.getnewaddress, address_type="silent-payment")

        if self.is_bdb_compiled():
            assert_raises_rpc_error(-4, "Wallet with silent payments must also be a descriptor wallet", self.nodes[0].createwallet, wallet_name="legacy_sp", descriptors=False, silent_payment=True)

            self.nodes[0].createwallet(wallet_name="legacy_sp", descriptors=False)
            wallet = self.nodes[0].get_wallet_rpc("legacy_sp")
            assert_raises_rpc_error(-12, "Error: No silent-payment addresses available", wallet.getnewaddress, address_type="silent-payment")

    def test_basic(self):
        self.log.info("Basic receive and send")

        self.nodes[0].createwallet(wallet_name="basic", silent_payment=True)
        wallet = self.nodes[0].get_wallet_rpc("basic")

        addr = wallet.getnewaddress(address_type="silent-payment")
        addr_again = wallet.getnewaddress(address_type="silent-payment")
        assert addr == addr_again
        txid = self.def_wallet.sendtoaddress(addr, 10)
        self.generate(self.nodes[0], 1)

        assert_equal(wallet.getbalance(), 10)
        wallet.gettransaction(txid)

        self.log.info("Test getnewaddress returns new labelled address")
        labeled_addr = wallet.getnewaddress(address_type="silent-payment", label="foo")
        assert labeled_addr != addr
        txid = self.def_wallet.sendtoaddress(labeled_addr, 10)
        self.generate(self.nodes[0], 1)

        assert_equal(wallet.getbalance(), 20)
        wallet.gettransaction(txid)

        self.log.info("Test self-transfer")
        txid = wallet.send({addr: 5})
        self.generate(self.nodes[0], 1)
        assert_approx(wallet.getbalance(), 20, 0.0001)

        wallet.sendall([self.def_wallet.getnewaddress()])
        self.generate(self.nodes[0], 1)

        assert_equal(wallet.getbalance(), 0)

    def test_import_rescan(self):
        self.log.info("Check import rescan works for silent payments")

        self.nodes[0].createwallet(wallet_name="alice", silent_payment=True)
        self.nodes[0].createwallet(wallet_name="alice_wo", disable_private_keys=True, silent_payment=True)
        alice = self.nodes[0].get_wallet_rpc("alice")
        alice_wo = self.nodes[0].get_wallet_rpc("alice_wo")

        address = alice.getnewaddress(address_type="silent-payment")
        self.def_wallet.sendtoaddress(address, 10)
        blockhash = self.generate(self.nodes[0], 1)[0]
        timestamp = self.nodes[0].getblockheader(blockhash)["time"]
        assert_approx(alice.getbalance(), 10, 0.0001)
        assert_equal(alice_wo.getbalance(), 0)

        alice_sp_desc = [d["desc"] for d in alice.listdescriptors()["descriptors"] if d["desc"].startswith("sp(")][0]
        alice_wo.importdescriptors([{
            "desc": alice_sp_desc,
            "active": True,
            "next_index": 0,
            "timestamp": timestamp
        }])

        assert_approx(alice_wo.getbalance(), 10, 0.0001)

    def test_rbf(self):
        self.log.info("Check Silent Payments RBF")

        self.nodes[0].createwallet(wallet_name="craig", silent_payment=True)
        wallet = self.nodes[0].get_wallet_rpc("craig")
        address = wallet.getnewaddress(address_type="silent-payment")

        txid = self.def_wallet.sendtoaddress(address, 49.99, replaceable=True)
        assert_equal(self.nodes[0].getrawmempool(), [txid])
        raw_tx = self.nodes[0].getrawtransaction(txid)
        tx = self.nodes[0].decoderawtransaction(raw_tx)
        assert_equal(len(tx["vin"]), 1)

        psbt = self.def_wallet.psbtbumpfee(txid, fee_rate=10000)["psbt"]
        decoded = self.nodes[0].decodepsbt(psbt)
        assert_equal(len(decoded["tx"]["vin"]), 2)

        res = self.def_wallet.walletprocesspsbt(psbt)
        assert_equal(res["complete"], True)
        txid = self.def_wallet.sendrawtransaction(res["hex"])
        assert_equal(self.nodes[0].getrawmempool(), [txid])

        assert_equal(wallet.getbalance(), 0)
        self.generate(self.nodes[0], 1)
        assert_approx(wallet.getbalance(), 49.99, 0.0001)

    def test_import_link_keys(self):
        self.log.info("Check wallet with key recognizes imported sppub")

        self.nodes[0].createwallet(wallet_name="base", silent_payment=True)
        base_wallet = self.nodes[0].get_wallet_rpc("base")
        hd_info = base_wallet.gethdkeys(private=True)
        xprv = hd_info[0]["xprv"]
        sppub_desc = [d["desc"] for d in base_wallet.listdescriptors()["descriptors"] if d["desc"].startswith("sp(")][0]
        spprv_desc = [d["desc"] for d in base_wallet.listdescriptors(private=True)["descriptors"] if d["desc"].startswith("sp(")][0]

        self.nodes[0].createwallet(wallet_name="blank", blank=True)
        wallet = self.nodes[0].get_wallet_rpc("blank")
        # Import this descriptor adds the xprv to the wallet
        assert_equal(wallet.importdescriptors([{"desc": descsum_create(f"pkh({xprv}/44h/2h/0h/0/0/*)"), "timestamp": "now", "active": True}])[0]["success"], True)
        # Fails here because we can't import a desc wuithout private keys to a wallet with private keys enabled
        assert_equal(wallet.importdescriptors([{"desc": sppub_desc, "timestamp": "now", "active": True}])[0]["success"], True)

        wallet_spprv_dec = [d["desc"] for d in base_wallet.listdescriptors(private=True)["descriptors"] if d["desc"].startswith("sp(")][0]
        assert_equal(wallet_spprv_dec, spprv_desc)

    def run_test(self):
        self.def_wallet = self.nodes[0].get_wallet_rpc(self.default_wallet_name)
        self.generate(self.nodes[0], 102)

        self.test_rbf()
        self.test_createwallet()
        self.test_encrypt_and_decrypt()
        self.test_encrypting_unencrypted()
        self.test_basic()
        self.test_import_rescan()
        self.test_import_link_keys()


if __name__ == '__main__':
    SilentPaymentsReceivingTest().main()
