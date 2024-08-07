/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996, 1997, 1998
 *	Sleepycat Software.  All rights reserved.
 */
/*
 * Copyright (c) 1996
 *	The President and Fellows of Harvard University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#ifndef lint
static const char sccsid[] = "@(#)txn_rec.c	10.15 (Sleepycat) 1/3/99";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <errno.h>
#endif

#include "db_int.h"
#include "db_page.h"
#include "shqueue.h"
#include "txn.h"
#include "db_am.h"
#include "log.h"
#include "common_ext.h"

static int __txn_restore_txn __P((DB_ENV *, DB_LSN *, __txn_xa_regop_args *));

#define	IS_XA_TXN(R) (R->xid.size != 0)

/*
 * PUBLIC: int __txn_regop_recover
 * PUBLIC:    __P((DB_LOG *, DBT *, DB_LSN *, int, void *));
 *
 * These records are only ever written for commits.
 */
int
__txn_regop_recover(logp, dbtp, lsnp, redo, info)
	DB_LOG *logp;
	DBT *dbtp;
	DB_LSN *lsnp;
	int redo;
	void *info;
{
	__txn_regop_args *argp;
	int ret;

#ifdef DEBUG_RECOVER
	(void)__txn_regop_print(logp, dbtp, lsnp, redo, info);
#endif
	COMPQUIET(redo, 0);
	COMPQUIET(logp, NULL);

	if ((ret = __txn_regop_read(dbtp->data, &argp)) != 0)
		return (ret);

	if (argp->opcode != TXN_COMMIT)
		ret = EINVAL;
	else
		if (__db_txnlist_find(info, argp->txnid->txnid) == DB_NOTFOUND)
			ret = __db_txnlist_add(info, argp->txnid->txnid);

	if (ret == 0)
		*lsnp = argp->prev_lsn;
	__os_free(argp, 0);

	return (ret);
}

/*
 * PUBLIC: int __txn_xa_regop_recover
 * PUBLIC:    __P((DB_LOG *, DBT *, DB_LSN *, int, void *));
 *
 * These records are only ever written for prepares.
 */
int
__txn_xa_regop_recover(logp, dbtp, lsnp, redo, info)
	DB_LOG *logp;
	DBT *dbtp;
	DB_LSN *lsnp;
	int redo;
	void *info;
{
	__txn_xa_regop_args *argp;
	int ret;

#ifdef DEBUG_RECOVER
	(void)__txn_xa_regop_print(logp, dbtp, lsnp, redo, info);
#endif
	COMPQUIET(redo, 0);
	COMPQUIET(logp, NULL);

	if ((ret = __txn_xa_regop_read(dbtp->data, &argp)) != 0)
		return (ret);

	if (argp->opcode != TXN_PREPARE)
		ret = EINVAL;
	else {
		/*
		 * Whether we are in XA or not, we need to call
		 * __db_txnlist_find so that we update the maxid.
		 * If this is an XA transaction, then we treat
		 * prepares like commits so that we roll forward to
		 * a point where we can handle commit/abort calls
		 * from the TMS.  If this isn't XA, then a prepare
		 * is treated like a No-op; we only care about the
		 * commit.
		 */
		ret = __db_txnlist_find(info, argp->txnid->txnid);
		if (IS_XA_TXN(argp) && ret == DB_NOTFOUND) {
			/*
			 * This is an XA prepared, but not yet committed
			 * transaction.  We need to add it to the
			 * transaction list, so that it gets rolled
			 * forward. We also have to add it to the region's
			 * internal state so it can be properly aborted
			 * or recovered.
			 */
			ret = __db_txnlist_add(info, argp->txnid->txnid);
			if (ret == 0)
				ret = __txn_restore_txn(logp->dbenv,
				    lsnp, argp);
		}
	}

	if (ret == 0)
		*lsnp = argp->prev_lsn;
	__os_free(argp, 0);

	return (ret);
}

/*
 * PUBLIC: int __txn_ckp_recover __P((DB_LOG *, DBT *, DB_LSN *, int, void *));
 */
int
__txn_ckp_recover(logp, dbtp, lsnp, redo, info)
	DB_LOG *logp;
	DBT *dbtp;
	DB_LSN *lsnp;
	int redo;
	void *info;
{
	__txn_ckp_args *argp;
	int ret;

#ifdef DEBUG_RECOVER
	__txn_ckp_print(logp, dbtp, lsnp, redo, info);
#endif
	COMPQUIET(logp, NULL);

	if ((ret = __txn_ckp_read(dbtp->data, &argp)) != 0)
		return (ret);

	/*
	 * Check for 'restart' checkpoint record.  This occurs when the
	 * checkpoint lsn is equal to the lsn of the checkpoint record
	 * and means that we could set the transaction ID back to 1, so
	 * that we don't exhaust the transaction ID name space.
	 */
	if (argp->ckp_lsn.file == lsnp->file &&
	    argp->ckp_lsn.offset == lsnp->offset)
		__db_txnlist_gen(info, redo ? -1 : 1);

	*lsnp = argp->last_ckp;
	__os_free(argp, 0);
	return (DB_TXN_CKP);
}

/*
 * __txn_child_recover
 *	Recover a commit record for a child transaction.
 *
 * PUBLIC: int __txn_child_recover
 * PUBLIC:    __P((DB_LOG *, DBT *, DB_LSN *, int, void *));
 */
int
__txn_child_recover(logp, dbtp, lsnp, redo, info)
	DB_LOG *logp;
	DBT *dbtp;
	DB_LSN *lsnp;
	int redo;
	void *info;
{
	__txn_child_args *argp;
	int ret;

#ifdef DEBUG_RECOVER
	(void)__txn_child_print(logp, dbtp, lsnp, redo, info);
#endif
	COMPQUIET(redo, 0);
	COMPQUIET(logp, NULL);

	if ((ret = __txn_child_read(dbtp->data, &argp)) != 0)
		return (ret);

	/*
	 * We count the child as committed only if its parent committed.
	 * So, if we are not yet in the transaction list, but our parent
	 * is, then we should go ahead and commit.
	 */
	if (argp->opcode != TXN_COMMIT)
		ret = EINVAL;
	else
		if (__db_txnlist_find(info, argp->parent) == 0 &&
		    __db_txnlist_find(info, argp->txnid->txnid) == DB_NOTFOUND)
			ret = __db_txnlist_add(info, argp->txnid->txnid);

	if (ret == 0)
		*lsnp = argp->prev_lsn;
	__os_free(argp, 0);

	return (ret);
}

/*
 * __txn_restore_txn --
 *	Using only during XA recovery.  If we find any transactions that are
 * prepared, but not yet committed, then we need to restore the transaction's
 * state into the shared region, because the TM is going to issue a txn_abort
 * or txn_commit and we need to respond correctly.
 *
 * lsnp is the LSN of the returned LSN
 * argp is the perpare record (in an appropriate structure)
 */
static int
__txn_restore_txn(dbenv, lsnp, argp)
	DB_ENV *dbenv;
	DB_LSN *lsnp;
	__txn_xa_regop_args *argp;
{
	DB_TXNMGR *mgr;
	TXN_DETAIL *td;
	int ret;

	if (argp->xid.size == 0)
		return(0);

	mgr = dbenv->tx_info;
	LOCK_TXNREGION(mgr);

	/* Allocate a new transaction detail structure. */
	if ((ret = __db_shalloc(mgr->mem, sizeof(TXN_DETAIL), 0, &td)) != 0)
		return (ret);

	/* Place transaction on active transaction list. */
	SH_TAILQ_INSERT_HEAD(&mgr->region->active_txn, td, links, __txn_detail);

	td->txnid = argp->txnid->txnid;
	td->begin_lsn = argp->begin_lsn;
	td->last_lsn = *lsnp;
	td->last_lock = 0;
	td->parent = 0;
	td->status = TXN_PREPARED;
	td->xa_status = TXN_XA_PREPARED;
	memcpy(td->xid, argp->xid.data, argp->xid.size);
	td->bqual = argp->bqual;
	td->gtrid = argp->gtrid;
	td->format = argp->formatID;

	UNLOCK_TXNREGION(mgr);
	return (0);
}
