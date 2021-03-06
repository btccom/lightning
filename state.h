#ifndef LIGHTNING_STATE_H
#define LIGHTNING_STATE_H
#include "config.h"

#include <ccan/tal/tal.h>
#include <state_types.h>
#include <stdbool.h>

/*
 * This is the core state machine.
 *
 * Calling the state machine updates updates peer->state, and may call
 * various peer_ callbacks.  It also returns the status of the current
 * command.
 */

static inline bool state_is_error(enum state s)
{
	return s >= STATE_ERR_ANCHOR_TIMEOUT && s <= STATE_ERR_INTERNAL;
}

struct peer;
struct bitcoin_tx;

static inline bool input_is_pkt(enum state_input input)
{
	return input <= PKT_ERROR;
}

union input {
	Pkt *pkt;
	struct command *cmd;
	struct bitcoin_event *btc;
	struct htlc *htlc;
	struct htlc_progress *htlc_prog;
};

enum command_status state(struct peer *peer,
			  const enum state_input input,
			  const union input *idata,
			  const struct bitcoin_tx **broadcast);

/* Any CMD_SEND_HTLC_* */
#define CMD_SEND_UPDATE_ANY INPUT_MAX

/* a == b?  (or one of several for CMD_SEND_UPDATE_ANY) */
static inline bool input_is(enum state_input a, enum state_input b)
{
	if (b == CMD_SEND_UPDATE_ANY) {
		/* Single | here, we want to record all. */
		return input_is(a, CMD_SEND_HTLC_ADD)
			| input_is(a, CMD_SEND_HTLC_FULFILL)
			| input_is(a, CMD_SEND_HTLC_FAIL);
	}

/* For test_state_coverate to make the states. */
#ifdef MAPPING_INPUTS
	MAPPING_INPUTS(b);
#endif
	return a == b;
}

struct signature;

/* Inform peer have an unexpected packet. */
void peer_unexpected_pkt(struct peer *peer, const Pkt *pkt);

/* An on-chain transaction revealed an R value. */
const struct htlc *peer_tx_revealed_r_value(struct peer *peer,
					    const struct bitcoin_event *btc);

/* Send various kinds of packets */
void queue_pkt_open(struct peer *peer, OpenChannel__AnchorOffer anchor);
void queue_pkt_anchor(struct peer *peer);
void queue_pkt_open_commit_sig(struct peer *peer);
void queue_pkt_open_complete(struct peer *peer);
void queue_pkt_htlc_add(struct peer *peer,
			const struct htlc_progress *htlc_prog);
void queue_pkt_htlc_fulfill(struct peer *peer,
			    const struct htlc_progress *htlc_prog);
void queue_pkt_htlc_fail(struct peer *peer,
			 const struct htlc_progress *htlc_prog);
void queue_pkt_commit(struct peer *peer);
void queue_pkt_revocation(struct peer *peer);
void queue_pkt_close_clearing(struct peer *peer);
void queue_pkt_close_signature(struct peer *peer);

Pkt *pkt_err(struct peer *peer, const char *msg, ...);
void queue_pkt_err(struct peer *peer, Pkt *err);
Pkt *pkt_err_unexpected(struct peer *peer, const Pkt *pkt);

/* Process various packets: return an error packet on failure. */
Pkt *accept_pkt_open(struct peer *peer, const Pkt *pkt);

Pkt *accept_pkt_anchor(struct peer *peer, const Pkt *pkt);

Pkt *accept_pkt_open_commit_sig(struct peer *peer, const Pkt *pkt);
	
Pkt *accept_pkt_open_complete(struct peer *peer, const Pkt *pkt);
	
Pkt *accept_pkt_htlc_add(struct peer *peer, const Pkt *pkt);

Pkt *accept_pkt_htlc_fail(struct peer *peer, const Pkt *pkt);

Pkt *accept_pkt_htlc_fulfill(struct peer *peer, const Pkt *pkt);

Pkt *accept_pkt_update_accept(struct peer *peer, const Pkt *pkt);

Pkt *accept_pkt_commit(struct peer *peer, const Pkt *pkt);

Pkt *accept_pkt_revocation(struct peer *peer, const Pkt *pkt);
Pkt *accept_pkt_close_clearing(struct peer *peer, const Pkt *pkt);
Pkt *accept_pkt_close_sig(struct peer *peer, const Pkt *pkt, bool *matches);

/**
 * committed_to_htlcs: do we have any locked-in HTLCs?
 * @peer: the state data for this peer.
 *
 * If we were to generate a commit tx now, would it have HTLCs in it?
 */
bool committed_to_htlcs(const struct peer *peer);

/**
 * peer_has_close_sig: do we have a valid close_sig from them?
 * @peer: the state data for this peer.
 *
 * We use any acceptable close tx, if we have one, in preference to a commit tx.
 */
bool peer_has_close_sig(const struct peer *peer);

/**
 * peer_watch_anchor: create a watch for the anchor transaction.
 * @peer: the state data for this peer.
 * @depthok: the input to give when anchor reaches expected depth.
 * @timeout: the input to give if anchor doesn't reach depth in time.
 * @unspent: the input to give if anchor is unspent after @depthok.
 * @theyspent: the input to give if they spend anchor with their commit tx.
 * @otherspent: the input to give if they spend anchor otherwise.
 *
 * @depthok can be INPUT_NONE if it's our anchor (we don't time
 * ourselves out).
 */
void peer_watch_anchor(struct peer *peer,
		       enum state_input depthok,
		       enum state_input timeout,
		       enum state_input unspent,
		       enum state_input theyspent,
		       enum state_input otherspent);

/**
 * peer_unwatch_anchor_depth: remove depth watch for the anchor.
 * @peer: the state data for this peer.
 * @depthok: the input to give when anchor reaches expected depth.
 * @timeout: the input to give if anchor doesn't reach depth in time.
 *
 * @depthok and @timeout must match bitcoin_watch_anchor() call.
 */
void peer_unwatch_anchor_depth(struct peer *peer,
			       enum state_input depthok,
			       enum state_input timeout);

/**
 * peer_watch_delayed: watch this (commit) tx, tell me when I can spend it
 * @peer: the state data for this peer.
 * @tx: the tx we're watching.
 * @canspend: the input to give when commit reaches spendable depth.
 *
 * Note that this tx may be malleated, as it's dual-signed.
 */
void peer_watch_delayed(struct peer *peer,
			const struct bitcoin_tx *tx,
			enum state_input canspend);

/**
 * peer_watch_tx: watch this tx until it's "irreversible"
 * @peer: the state data for this peer.
 * @tx: the tx we're watching.
 * @done: the input to give when tx is completely buried.
 *
 * Once this fires we consider the channel completely closed and stop
 * watching (eg 100 txs down).
 *
 * This is used for watching a transaction we sent (such as a steal,
 * or spend of their close, etc).
 */
void peer_watch_tx(struct peer *peer,
		   const struct bitcoin_tx *tx,
		   enum state_input done);

/**
 * peer_watch_close: watch for close tx until it's "irreversible" (or timedout)
 * @peer: the state data for this peer.
 * @done: the input to give when tx is completely buried.
 * @timedout: the input to give if we time out (they don't provide sig).
 *
 * Once this fires we consider the channel completely closed and stop
 * watching (eg 100 txs down).
 *
 * This is used for watching a mutual close.
 */
void peer_watch_close(struct peer *peer,
		      enum state_input done, enum state_input timedout);

/**
 * peer_unwatch_close_timeout: remove timeout for the close transaction
 * @peer: the state data for this peer.
 * @timeout: the input to give if anchor doesn't reach depth in time.
 *
 * This is called once we have successfully received their signature.
 */
void peer_unwatch_close_timeout(struct peer *peer, enum state_input timedout);

/**
 * peer_watch_anchor: create a watch for the anchor transaction.
 * @peer: the state data for this peer.
 * @depthok: the input to give when anchor reaches expected depth.
 * @timeout: the input to give if anchor doesn't reach depth in time.
 * @unspent: the input to give if anchor is unspent after @depthok.
 * @theyspent: the input to give if they spend anchor with their commit tx.
 * @otherspent: the input to give if they spend anchor otherwise.
 *
 * @depthok can be INPUT_NONE if it's our anchor (we don't time
 * ourselves out).
 */
void peer_unwatch_close_timeout(struct peer *peer, enum state_input timedout);

/**
 * peer_watch_our_htlc_outputs: HTLC outputs from our commit tx to watch.
 * @peer: the state data for this peer.
 * @tx: the commitment tx
 * @tous_timeout: input to give when a HTLC output to us times out.
 * @tothem_spent: input to give when a HTLC output to them is spent.
 * @tothem_timeout: input to give when a HTLC output to them times out.
 *
 * Returns true if there were any htlc outputs to watch.
 */
bool peer_watch_our_htlc_outputs(struct peer *peer,
				 const struct bitcoin_tx *tx,
				 enum state_input tous_timeout,
				 enum state_input tothem_spent,
				 enum state_input tothem_timeout);

/**
 * peer_watch_their_htlc_outputs: HTLC outputs from their commit tx to watch.
 * @peer: the state data for this peer.
 * @tx: the commitment tx
 * @tous_timeout: input to give when a HTLC output to us times out.
 * @tothem_spent: input to give when a HTLC output to them is spent.
 * @tothem_timeout: input to give when a HTLC output to them times out.
 *
 * Returns true if there were any htlc outputs to watch.
 */
bool peer_watch_their_htlc_outputs(struct peer *peer,
				   const struct bitcoin_event *tx,
				   enum state_input tous_timeout,
				   enum state_input tothem_spent,
				   enum state_input tothem_timeout);

/**
 * peer_unwatch_htlc_output: stop watching an HTLC
 * @peer: the state data for this peer.
 * @htlc: the htlc to stop watching
 * @all_done: input to give if we're not watching any outputs anymore.
 */
void peer_unwatch_htlc_output(struct peer *peer,
			      const struct htlc *htlc,
			      enum state_input all_done);

/**
 * peer_unwatch_all_htlc_outputs: stop watching all HTLCs
 * @peer: the state data for this peer.
 */
void peer_unwatch_all_htlc_outputs(struct peer *peer);

/**
 * peer_watch_htlc_spend: watch our spend of an HTLC output
 * @peer: the state data for this peer.
 * @tx: the commitment tx
 * @htlc: the htlc the tx is spending an output of
 * @done: input to give when it's completely buried.
 */
void peer_watch_htlc_spend(struct peer *peer,
			   const struct bitcoin_tx *tx,
			   const struct htlc *htlc,
			   enum state_input done);

/**
 * peer_unwatch_htlc_spend: stop watching our HTLC spend
 * @peer: the state data for this peer.
 * @htlc: the htlc to stop watching the spend for.
 * @all_done: input to give if we're not watching anything anymore.
 */
void peer_unwatch_htlc_spend(struct peer *peer,
			     const struct htlc *htlc,
			     enum state_input all_done);

/**
 * peer_watch_htlcs_cleared: tell us when no HTLCs are in commit txs.
 * @peer: the state data for this peer.
 * @all_done: input to give when all HTLCs are done.
 */
void peer_watch_htlcs_cleared(struct peer *peer,
			      enum state_input all_done);

/**
 * peer_calculate_close_fee: figure out what the fee for closing is.
 * @peer: the state data for this peer.
 */
void peer_calculate_close_fee(struct peer *peer);

/* Start creation of the bitcoin anchor tx. */
void bitcoin_create_anchor(struct peer *peer, enum state_input done);

/* We didn't end up broadcasting the anchor: release the utxos.
 * If done != INPUT_NONE, remove existing create_anchor too. */
void bitcoin_release_anchor(struct peer *peer, enum state_input done);

/* Get the bitcoin anchor tx. */
const struct bitcoin_tx *bitcoin_anchor(struct peer *peer);

/* Create a bitcoin close tx. */
const struct bitcoin_tx *bitcoin_close(struct peer *peer);

/* Create a bitcoin spend tx (to spend our commit's outputs) */
const struct bitcoin_tx *bitcoin_spend_ours(struct peer *peer);

/* Create a bitcoin spend tx (to spend their commit's outputs) */
const struct bitcoin_tx *bitcoin_spend_theirs(const struct peer *peer,
					      const struct bitcoin_event *btc);

/* Create a bitcoin steal tx (to steal all their commit's outputs) */
const struct bitcoin_tx *bitcoin_steal(const struct peer *peer,
				       struct bitcoin_event *btc);

/* Create our commit tx */
const struct bitcoin_tx *bitcoin_commit(struct peer *peer);

/* Create a HTLC refund collection */
const struct bitcoin_tx *bitcoin_htlc_timeout(const struct peer *peer,
					      const struct htlc *htlc);

/* Create a HTLC collection */
const struct bitcoin_tx *bitcoin_htlc_spend(const struct peer *peer,
					    const struct htlc *htlc);

#endif /* LIGHTNING_STATE_H */
