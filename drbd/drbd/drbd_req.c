/*
-*- linux-c -*-
   drbd.c
   Kernel module for 2.4.x Kernels

   This file is part of drbd by Philipp Reisner.

   Copyright (C) 1999-2003, Philipp Reisner <philipp.reisner@gmx.at>.
	main author.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#ifdef HAVE_AUTOCONF
#include <linux/autoconf.h>
#endif
#ifdef CONFIG_MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/slab.h>
#include "drbd.h"
#include "drbd_int.h"

void drbd_end_req(drbd_request_t *req, int nextstate, int er_flags,
		  sector_t rsector)
{
	/* This callback will be called in irq context by the IDE drivers,
	   and in Softirqs/Tasklets/BH context by the SCSI drivers.
	   This function is called by the receiver in kernel-thread context.
	   Try to get the locking right :) */

	struct Drbd_Conf* mdev = drbd_conf + MINOR(req->bh->b_rdev);
	unsigned long flags=0;

	PARANOIA_BUG_ON(req->pbh.b_blocknr != rsector);
	spin_lock_irqsave(&mdev->req_lock,flags);

	if(req->rq_status & nextstate) {
		ERR("request state error(%d)\n", req->rq_status);
	}

	req->rq_status |= nextstate;
	req->rq_status &= er_flags | ~0x0001;
	if( (req->rq_status & RQ_DRBD_DONE) == RQ_DRBD_DONE ) goto end_it;

	spin_unlock_irqrestore(&mdev->req_lock,flags);

	return;

/* We only report uptodate == TRUE if both operations (WRITE && SEND)
   reported uptodate == TRUE
 */

	end_it:
	spin_unlock_irqrestore(&mdev->req_lock,flags);

	if( ! ( er_flags & ERF_NOTLD ) ) {
		/*If this call is from tl_clear() we may not call tl_dependene,
		  otherwhise we have a homegrown spinlock deadlock.   */
		if(tl_dependence(mdev,req))
			set_bit(ISSUE_BARRIER,&mdev->flags);
	} else {
		list_del(&req->w.list); // we have the tl_lock...
	}

	if(mdev->conf.wire_protocol==DRBD_PROT_C && mdev->cstate > Connected) {
		drbd_set_in_sync(mdev,rsector,req->bh->b_size,0);
	}

	req->bh->b_end_io(req->bh,(req->rq_status & 0x0001));

	if( mdev->do_panic && !(req->rq_status & 0x0001) ) {
		drbd_panic(DEVICE_NAME": The lower-level device had an error.\n");
	}

	INVALIDATE_MAGIC(req);
	mempool_free(req,drbd_request_mempool);

	if (test_bit(ISSUE_BARRIER,&mdev->flags))
		wake_asender(mdev);
}

/*
 * b_end_io for writes on Primary comming from drbd_make_request
 */
void drbd_dio_end(struct buffer_head *bh, int uptodate)
{
	struct Drbd_Conf* mdev;
	drbd_request_t *req;

	// ok, now we have the b_private available for other use
	req = container_of(bh,struct drbd_request,pbh);
	PARANOIA_BUG_ON(!VALID_POINTER(req));
	mdev = drbd_conf+MINOR(req->bh->b_rdev);
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	// NOT bh->b_rsector, may have been remapped!
	drbd_end_req(req, RQ_DRBD_WRITTEN, uptodate, req->bh->b_rsector);
	drbd_al_complete_io(mdev,req->bh->b_rsector);
}

STATIC struct Pending_read*
drbd_find_read(sector_t sector, struct list_head *in)
{
	struct list_head *le;
	struct Pending_read *pr;

	list_for_each(le,in) {
		pr = list_entry(le, struct Pending_read, w.list);
		if(pr->d.sector == sector) return pr;
	}

	return NULL;
}

STATIC void drbd_issue_drequest(struct Drbd_Conf* mdev,struct buffer_head *bh)
{
	struct Pending_read *pr;
	pr = mempool_alloc(drbd_pr_mempool, GFP_DRBD);

	if (!pr) {
		ERR("could not kmalloc() pr\n");
		bh->b_end_io(bh,0);
		return;
	}
	SET_MAGIC(pr);

	pr->d.bh = bh;
	pr->cause = mdev->cstate == SyncTarget ? AppAndResync : Application;
	spin_lock(&mdev->pr_lock);
	list_add(&pr->w.list,&mdev->app_reads);
	spin_unlock(&mdev->pr_lock);
	inc_pending(mdev);
	drbd_send_drequest(mdev, mdev->cstate == SyncTarget ? RSDataRequest : DataRequest,
			   bh->b_rsector, bh->b_size,
			   (unsigned long)pr);
}


int drbd_make_request(request_queue_t *q, int rw, struct buffer_head *bh)
{
	struct Drbd_Conf* mdev = drbd_conf + MINOR(bh->b_rdev);
	struct buffer_head *nbh;
	drbd_request_t *req;
	int send_ok;

	if( mdev->lo_device == 0 ) {
		if( mdev->cstate < Connected ) {
			bh->b_end_io(bh,0);
			return 0;
		}

		if(!test_and_set_bit(WRITE_HINT_QUEUED,&mdev->flags)) {
			queue_task(&mdev->write_hint_tq, &tq_disk); // IO HINT
		}

		// Fail READA ??
		if( rw == WRITE ) {
			req = mempool_alloc(drbd_request_mempool, GFP_DRBD);

			if (!req) {
				ERR("could not kmalloc() req\n");
				bh->b_end_io(bh,0);
				return 0;
			}
			SET_MAGIC(req);

			req->rq_status = RQ_DRBD_WRITTEN | 1;
			req->bh=bh;

			if(mdev->conf.wire_protocol != DRBD_PROT_A) {
				inc_pending(mdev);
			}
			drbd_send_dblock(mdev,req); // FIXME error check?
		} else { // rw == READ || rw == READA
			drbd_issue_drequest(mdev,bh);
		}
		return 0; // Ok everything arranged
	}

	if( mdev->cstate == SyncTarget &&
	    bm_get_bit(mdev->mbds_id,bh->b_rsector,bh->b_size) ) {
		struct Pending_read *pr;
		if( rw == WRITE ) {
			spin_lock(&mdev->pr_lock);
			pr=drbd_find_read(bh->b_rsector,&mdev->resync_reads);

			if(pr) {
				ERR("Will discard a resync_read\n");
				pr->cause = Discard;
				// list del as well ?
			}
			spin_unlock(&mdev->pr_lock);

			// TODO wait until writes of syncer are done.
			// Continue with a mirrored write op.
			// Set some flag to clear it in the bitmap
		} else { // rw == READ || rw == READA
			spin_lock(&mdev->pr_lock);
			pr=drbd_find_read(bh->b_rsector,&mdev->resync_reads);
			if(pr) {
				ERR("Upgraded a resync read to an app read\n");

				pr->cause |= Application;
				pr->d.bh=bh;
				list_del(&pr->w.list);
				list_add(&pr->w.list,&mdev->app_reads);
				spin_unlock(&mdev->pr_lock);
				return 0; // Ok everything arranged
			}

			spin_unlock(&mdev->pr_lock);
			drbd_issue_drequest(mdev,bh);
			return 0;
		}
	}

	if( rw == READ || rw == READA ) {
		mdev->read_cnt+=bh->b_size>>9;

		bh->b_rdev = mdev->lo_device;
		return 1; // Not arranged for transfer ( but remapped :)
	}

	mdev->writ_cnt+=bh->b_size>>9;

	if(mdev->cstate<Connected || test_bit(PARTNER_DISKLESS,&mdev->flags)) {
		drbd_set_out_of_sync(mdev,bh->b_rsector,bh->b_size);

		drbd_al_begin_io(mdev, bh->b_rsector);
		drbd_al_complete_io(mdev, bh->b_rsector); // FIXME TODO
		bh->b_rdev = mdev->lo_device;
		return 1; // Not arranged for transfer ( but remapped :)
	}

	// Now its clear that we have to do a mirrored write:

	req = mempool_alloc(drbd_request_mempool, GFP_DRBD);

	if (!req) {
		ERR("could not kmalloc() req\n");
		bh->b_end_io(bh,0);
		return 0;
	}
	SET_MAGIC(req);

	nbh = &req->pbh;

	drbd_init_bh(nbh, bh->b_size);

	nbh->b_page=bh->b_page; // instead of set_bh_page()
	nbh->b_data=bh->b_data; // instead of set_bh_page()

	drbd_set_bh(mdev, nbh, bh->b_rsector, bh->b_size);

	if(mdev->cstate < StandAlone || MINOR(bh->b_rdev) >= minor_count) {
		buffer_IO_error(bh);
		return 0;
	}

	nbh->b_private = req;
	nbh->b_state = (1 << BH_Dirty) | ( 1 << BH_Mapped) | (1 << BH_Lock);

	req->bh=bh;

	req->rq_status = RQ_DRBD_NOTHING;

	send_ok=drbd_send_dblock(mdev,req);
	// FIXME we could remove the send_ok cases, the are redundant to tl_clear()
	if(send_ok && mdev->conf.wire_protocol!=DRBD_PROT_A) inc_pending(mdev);
	if(mdev->conf.wire_protocol==DRBD_PROT_A || (!send_ok) ) {
				/* If sending failed, we can not expect
				   an ack packet. */
		drbd_end_req(req, RQ_DRBD_SENT, 1, bh->b_rsector);
	}
	if(!send_ok) drbd_set_out_of_sync(mdev,bh->b_rsector,bh->b_size);

	if(!test_and_set_bit(WRITE_HINT_QUEUED,&mdev->flags)) {
		queue_task(&mdev->write_hint_tq, &tq_disk);
	}

	drbd_al_begin_io(mdev, nbh->b_rsector);

	nbh->b_end_io = drbd_dio_end;
	generic_make_request(rw,nbh);

	return 0; /* Ok, bh arranged for transfer */

}

