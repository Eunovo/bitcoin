#!/usr/bin/env python3

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.descriptors import descsum_create
from test_framework.messages import COIN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_not_equal,
    assert_equal
)
import re


class SilentPaymentsTransactTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [[], ["-txindex=1"], []]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def init_wallet(self, *, node):
        pass

    def test_send_between_wallets(self):
        pass

    def test_default_wallet_descriptor_format(self):
        self.log.info(
            "Testing default wallet descriptors match chain"
        )
        node = self.nodes[0]
        node.getblockchaininfo()

        node.createwallet(
            wallet_name="default_wallet",
            descriptors=True,
            disable_private_keys=False,
            blank=False,
            silent_payments=True,
        )
        default_wallet = node.get_wallet_rpc("default_wallet")

        descriptors = default_wallet.listdescriptors()['descriptors']

        found_sp_descriptor = False
        for item in descriptors:
            assert_not_equal(item['desc'], '')
            assert item['timestamp'] is not None
            assert_equal(item['active'], True)
            if item["desc"].startswith("sp("):
                found_sp_descriptor = True
                assert_equal(item['internal'], True)
                desc = item['desc']
                self.log.info(f"generated descriptor: {desc}")
                assert_not_equal(re.search(r"sp\(\[\w{8}/352h/1h/", desc), None)

        assert_equal(found_sp_descriptor, True)

    def test_import_sp_descriptor_with_private_keys(self):
        self.log.info(
            "Testing import silent payments wallet descriptors with private keys"
        )
        node = self.nodes[0]

        # create wallet with private keys and silent payments support
        node.createwallet(wallet_name="wallet_source", disable_private_keys=False, silent_payments=True)
        wallet_source = node.get_wallet_rpc("wallet_source")
        
        # fetch sp descriptor with private keys
        wallet_sp_desc = [d["desc"] for d in wallet_source.listdescriptors(True)["descriptors"] if d["desc"].startswith("sp(")][0]
        self.log.debug(f"sp descriptor to import {wallet_sp_desc}")
        # create blank wallet to import descriptor into
        node.createwallet(wallet_name='wallet_import', disable_private_keys=False, silent_payments=True, blank=True)
        wallet_import = node.get_wallet_rpc('wallet_import')
        responses = wallet_import.importdescriptors([{
            "desc": wallet_sp_desc,
            "active": True,
            "timestamp": "now"
        }])
        
        # verify import status
        for response in responses:
            assert_equal(response["success"], True)
            if "warnings" in response:
                for warning in response["warnings"]:
                    assert "Not all private keys provided" not in warning
        
        imp_addr = wallet_import.getnewaddress(address_type="silent-payments")
        assert imp_addr
        self.log.info(f"SP Address from imported descriptor {imp_addr}")


    def test_spend_restored_wallet_to_sp_wallet(self):
        self.log.info("Testing spend from restored wallet")
        node = self.nodes[0]
        
        node.createwallet(wallet_name='sender', disable_private_keys=False, silent_payments=True)
        node.createwallet(wallet_name='receiver', disable_private_keys=False, silent_payments=True)

        sender = node.get_wallet_rpc("sender")
        receiver = node.get_wallet_rpc("receiver")
        
        # Give sender some money
        self.generatetoaddress(
            node, COINBASE_MATURITY + 10, sender.getnewaddress()
        )

        # Backup the wallet
        backup_file = node.datadir_path / "sp_backup.bak"
        sender.backupwallet(str(backup_file))

        # Restore the wallet
        self.nodes[1].restorewallet(
            wallet_name="sender_restored",
            backup_file=str(backup_file)
        )
        # redefine sender
        sender = self.nodes[1].get_wallet_rpc("sender_restored")
        
        to_addr = receiver.getnewaddress(address_type="silent-payments")
        self.log.info(f"send money to receiver address:{to_addr}")
        txid = sender.sendtoaddress(to_addr, 2.01)
        assert txid

        self.sync_mempools(wait=0.1, nodes=[node, self.nodes[1]])
        self.generate(node, 1)

        assert float(receiver.getbalance()) == 2.01
        self.log.info(f"new wallet balances: sender:{sender.getbalance()}, receiver:{receiver.getbalance()}")

    def test_sp_coin_control(self):
        self.log.info("Testing Silent Payments coin control")
        
        node = self.nodes[0]
        node.createwallet("cc_sender", silent_payments=True)
        node.createwallet("cc_receiver", silent_payments=True)
        
        sender = node.get_wallet_rpc("cc_sender")
        receiver = node.get_wallet_rpc("cc_receiver")
        
        # Create multiple UTXOs
        for i in range(3):
            self.generatetoaddress(node, 50, sender.getnewaddress())
        
        sp_addr = receiver.getnewaddress(address_type="silent-payments")
        
        # Get specific UTXO to use
        unspent = sender.listunspent()
        
        # select utxos to spend
        inputs = []
        TOTAL_SPEND = 10
        total_amount_selected = 0
        for utxo in unspent:
            if total_amount_selected > TOTAL_SPEND:
                break
            inputs.append({"txid": utxo["txid"], "vout": utxo["vout"]})
            total_amount_selected += utxo["amount"]

        options = {
            "inputs": inputs,
            "add_to_wallet": True
        }
        
        result = sender.send(outputs={sp_addr: TOTAL_SPEND}, options=options)
        assert result["txid"]
        
        # Verify transaction used the selected input
        raw_tx = receiver.getrawtransaction(result["txid"], True)
        assert any(vin["txid"] == input["txid"] and 
                  vin["vout"] == input["vout"] 
                  for vin in raw_tx["vin"]
                  for input in inputs)

    def run_test(self):
        #### working tests ####
        self.test_default_wallet_descriptor_format()
        self.test_spend_restored_wallet_to_sp_wallet()

        #### broken tests ####
        # import private keys is broken
        self.test_import_sp_descriptor_with_private_keys()
        # send works without the following in spend.cpp
            # if (wallet.IsMine(*tx, spent_coins))
            #   wallet.WalletLogPrintf("Detected Silent Payments self-transfer: %s", tx->GetHash().ToString());
        self.test_sp_coin_control()


if __name__ == "__main__":
    SilentPaymentsTransactTest(__file__).main()
