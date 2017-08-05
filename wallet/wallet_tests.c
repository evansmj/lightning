#include "wallet.c"

#include <ccan/mem/mem.h>
#include "db.c"
#include "wallet/test_utils.h"

#include <stdio.h>
#include <unistd.h>

static struct wallet *create_test_wallet(const tal_t *ctx)
{
	char filename[] = "/tmp/ldb-XXXXXX";
	int fd = mkstemp(filename);
	struct wallet *w = tal(ctx, struct wallet);
	CHECK_MSG(fd != -1, "Unable to generate temp filename");
	close(fd);

	w->db = db_open(w, filename);

	CHECK_MSG(w->db, "Failed opening the db");
	CHECK_MSG(db_migrate(w->db), "DB migration failed");

	return w;
}

static bool test_wallet_outputs(void)
{
	char filename[] = "/tmp/ldb-XXXXXX";
	struct utxo u;
	int fd = mkstemp(filename);
	struct wallet *w = tal(NULL, struct wallet);
	CHECK_MSG(fd != -1, "Unable to generate temp filename");
	close(fd);

	w->db = db_open(w, filename);
	CHECK_MSG(w->db, "Failed opening the db");
	CHECK_MSG(db_migrate(w->db), "DB migration failed");

	memset(&u, 0, sizeof(u));

	/* Should work, it's the first time we add it */
	CHECK_MSG(wallet_add_utxo(w, &u, p2sh_wpkh),
		  "wallet_add_utxo failed on first add");

	/* Should fail, we already have that UTXO */
	CHECK_MSG(!wallet_add_utxo(w, &u, p2sh_wpkh),
		  "wallet_add_utxo succeeded on second add");

	/* Attempt to reserve the utxo */
	CHECK_MSG(wallet_update_output_status(w, &u.txid, u.outnum,
					      output_state_available,
					      output_state_reserved),
		  "could not reserve available output");

	/* Reserving twice should fail */
	CHECK_MSG(!wallet_update_output_status(w, &u.txid, u.outnum,
					       output_state_available,
					       output_state_reserved),
		  "could reserve already reserved output");

	/* Un-reserving should work */
	CHECK_MSG(wallet_update_output_status(w, &u.txid, u.outnum,
					      output_state_reserved,
					      output_state_available),
		  "could not unreserve reserved output");

	/* Switching from any to something else */
	CHECK_MSG(wallet_update_output_status(w, &u.txid, u.outnum,
					      output_state_any,
					      output_state_spent),
		  "could not change output state ignoring oldstate");

	tal_free(w);
	return true;
}

static bool test_shachain_crud(void)
{
	struct wallet_shachain a, b;
	char filename[] = "/tmp/ldb-XXXXXX";
	int fd = mkstemp(filename);
	struct wallet *w = tal(NULL, struct wallet);
	struct sha256 seed, hash;
	shachain_index_t index = UINT64_MAX >> (64 - SHACHAIN_BITS);

	w->db = db_open(w, filename);
	CHECK_MSG(w->db, "Failed opening the db");
	CHECK_MSG(db_migrate(w->db), "DB migration failed");

	CHECK_MSG(fd != -1, "Unable to generate temp filename");
	close(fd);
	memset(&seed, 'A', sizeof(seed));

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));

	w->db = db_open(w, filename);
	CHECK(wallet_shachain_init(w, &a));

	CHECK(a.id == 1);

	CHECK(a.chain.num_valid == 0 && a.chain.min_index == 0);

	for (int i=0; i<100; i++) {
		shachain_from_seed(&seed, index, &hash);
		CHECK(wallet_shachain_add_hash(w, &a, index, &hash));
		index--;
	}

	CHECK(wallet_shachain_load(w, a.id, &b));
	CHECK_MSG(memcmp(&a, &b, sizeof(a)) == 0, "Loading from database doesn't match");
	tal_free(w);
	return true;
}

static bool channelseq(struct wallet_channel *c1, struct wallet_channel *c2)
{
	struct peer *p1 = c1->peer, *p2 = c2->peer;
	struct channel_info *ci1 = p1->channel_info, *ci2 = p2->channel_info;
	struct changed_htlc *lc1 = p1->last_sent_commit, *lc2 = p2->last_sent_commit;
	CHECK(c1->id == c2->id);
	CHECK(c1->peer_id == c2->peer_id);
	CHECK(p1->their_shachain.id == p2->their_shachain.id);
	CHECK_MSG(pubkey_eq(&p1->id, &p2->id), "NodeIDs do not match");
	CHECK((p1->scid == NULL && p2->scid == NULL) || short_channel_id_eq(p1->scid, p2->scid));
	CHECK((p1->our_msatoshi == NULL && p2->our_msatoshi == NULL) || *p1->our_msatoshi == *p2->our_msatoshi);
	CHECK((p1->remote_shutdown_scriptpubkey == NULL && p2->remote_shutdown_scriptpubkey == NULL) || memeq(
		      p1->remote_shutdown_scriptpubkey,
		      tal_len(p1->remote_shutdown_scriptpubkey),
		      p2->remote_shutdown_scriptpubkey,
		      tal_len(p2->remote_shutdown_scriptpubkey)));
	CHECK((p1->funding_txid == NULL && p2->funding_txid == NULL) || memeq(
		      p1->funding_txid,
		      sizeof(struct sha256_double),
		      p2->funding_txid,
		      sizeof(struct sha256_double)));
	CHECK((ci1 != NULL) ==  (ci2 != NULL));
	if(ci1) {
		CHECK(memeq(&ci1->commit_sig, sizeof(secp256k1_ecdsa_signature),
			    &ci2->commit_sig, sizeof(secp256k1_ecdsa_signature)));
		CHECK(pubkey_eq(&ci1->remote_fundingkey, &ci2->remote_fundingkey));
		CHECK(pubkey_eq(&ci1->theirbase.revocation, &ci2->theirbase.revocation));
		CHECK(pubkey_eq(&ci1->theirbase.payment, &ci2->theirbase.payment));
		CHECK(pubkey_eq(&ci1->theirbase.delayed_payment, &ci2->theirbase.delayed_payment));
		CHECK(pubkey_eq(&ci1->remote_per_commit, &ci2->remote_per_commit));
		CHECK(pubkey_eq(&ci1->old_remote_per_commit, &ci2->old_remote_per_commit));
	}

	CHECK((lc1 != NULL) ==  (lc2 != NULL));
	if(lc1) {
		CHECK(lc1->newstate == lc2->newstate);
		CHECK(lc1->id == lc2->id);
	}

	return true;
}

static bool test_channel_crud(void)
{
	char filename[] = "/tmp/ldb-XXXXXX";
	int fd = mkstemp(filename);
	struct wallet *w = tal(NULL, struct wallet);
	struct wallet_channel c1, *c2 = tal(w, struct wallet_channel);
	struct peer p;
	struct channel_info ci;
	struct sha256_double *hash = tal(w, struct sha256_double);
	struct pubkey pk;
	struct changed_htlc last_commit;

	u64 msat = 12345;

	w->db = db_open(w, filename);
	CHECK_MSG(w->db, "Failed opening the db");
	CHECK_MSG(db_migrate(w->db), "DB migration failed");

	CHECK_MSG(fd != -1, "Unable to generate temp filename");
	close(fd);

	memset(&c1, 0, sizeof(c1));
	memset(c2, 0, sizeof(*c2));
	memset(&p, 0, sizeof(p));
	memset(&ci, 3, sizeof(ci));
	memset(hash, 'B', sizeof(*hash));
	memset(&last_commit, 0, sizeof(last_commit));
	pubkey_from_der(tal_hexdata(w, "02a1633cafcc01ebfb6d78e39f687a1f0995c62fc95f51ead10a02ee0be551b5dc", 66), 33, &pk);
	ci.feerate_per_kw = 31337;
	memset(&p.id, 'A', sizeof(p.id));
	c1.peer = &p;
	p.id = pk;
	p.unique_id = 42;
	p.our_msatoshi = NULL;
	ci.remote_fundingkey = pk;
	ci.theirbase.revocation = pk;
	ci.theirbase.payment = pk;
	ci.theirbase.delayed_payment = pk;
	ci.remote_per_commit = pk;
	ci.old_remote_per_commit = pk;

	/* Variant 1: insert with null for scid, funding_tx_id, and channel_info */
	CHECK_MSG(wallet_channel_save(w, &c1), tal_fmt(w, "Insert into DB: %s", w->db->err));
	CHECK_MSG(wallet_channel_load(w, c1.id, c2), tal_fmt(w, "Load from DB: %s", w->db->err));
	CHECK_MSG(channelseq(&c1, c2), "Compare loaded with saved (v1)");

	/* We just inserted them into an empty DB so this must be 1 */
	CHECK(c1.id == 1);
	CHECK(c1.peer_id == 1);
	CHECK(c1.peer->their_shachain.id == 1);

	/* Variant 2: update with scid set */
	c1.peer->scid = talz(w, struct short_channel_id);
	CHECK_MSG(wallet_channel_save(w, &c1), tal_fmt(w, "Insert into DB: %s", w->db->err));
	CHECK_MSG(wallet_channel_load(w, c1.id, c2), tal_fmt(w, "Load from DB: %s", w->db->err));
	CHECK_MSG(channelseq(&c1, c2), "Compare loaded with saved (v2)");

	/* Updates should not result in new ids */
	CHECK(c1.id == 1);
	CHECK(c1.peer_id == 1);
	CHECK(c1.peer->their_shachain.id == 1);

	/* Variant 3: update with our_satoshi set */
	c1.peer->our_msatoshi = &msat;
	CHECK_MSG(wallet_channel_save(w, &c1), tal_fmt(w, "Insert into DB: %s", w->db->err));
	CHECK_MSG(wallet_channel_load(w, c1.id, c2), tal_fmt(w, "Load from DB: %s", w->db->err));
	CHECK_MSG(channelseq(&c1, c2), "Compare loaded with saved (v3)");

	/* Variant 4: update with funding_tx_id */
	c1.peer->funding_txid = hash;
	CHECK_MSG(wallet_channel_save(w, &c1), tal_fmt(w, "Insert into DB: %s", w->db->err));
	CHECK_MSG(wallet_channel_load(w, c1.id, c2), tal_fmt(w, "Load from DB: %s", w->db->err));
	CHECK_MSG(channelseq(&c1, c2), "Compare loaded with saved (v4)");

	/* Variant 5: update with channel_info */
	p.channel_info = &ci;
	CHECK_MSG(wallet_channel_save(w, &c1), tal_fmt(w, "Insert into DB: %s", w->db->err));
	CHECK_MSG(wallet_channel_load(w, c1.id, c2), tal_fmt(w, "Load from DB: %s", w->db->err));
	CHECK_MSG(channelseq(&c1, c2), "Compare loaded with saved (v5)");

	/* Variant 6: update with last_commit_sent */
	p.last_sent_commit = &last_commit;
	CHECK_MSG(wallet_channel_save(w, &c1), tal_fmt(w, "Insert into DB: %s", w->db->err));
	CHECK_MSG(wallet_channel_load(w, c1.id, c2), tal_fmt(w, "Load from DB: %s", w->db->err));
	CHECK_MSG(channelseq(&c1, c2), "Compare loaded with saved (v6)");

	tal_free(w);
	return true;
}

static bool test_channel_config_crud(const tal_t *ctx)
{
	struct channel_config *cc1 = talz(ctx, struct channel_config),
			      *cc2 = talz(ctx, struct channel_config);
	struct wallet *w = create_test_wallet(ctx);
	CHECK(w);

	cc1->dust_limit_satoshis = 1;
	cc1->max_htlc_value_in_flight_msat = 2;
	cc1->channel_reserve_satoshis = 3;
	cc1->htlc_minimum_msat = 4;
	cc1->to_self_delay = 5;
	cc1->max_accepted_htlcs = 6;

	CHECK(wallet_channel_config_save(w, cc1));
	CHECK_MSG(
	    cc1->id == 1,
	    tal_fmt(ctx, "channel_config->id != 1; got %" PRIu64, cc1->id));

	CHECK(wallet_channel_config_load(w, cc1->id, cc2));
	CHECK(memeq(cc1, sizeof(*cc1), cc2, sizeof(*cc2)));
       	return true;
}

int main(void)
{
	bool ok = true;
	tal_t *tmpctx = tal_tmpctx(NULL);

	ok &= test_wallet_outputs();
	ok &= test_shachain_crud();
	ok &= test_channel_crud();
	ok &= test_channel_config_crud(tmpctx);

	tal_free(tmpctx);
	return !ok;
}
